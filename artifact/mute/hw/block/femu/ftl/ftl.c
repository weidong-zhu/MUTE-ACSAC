#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "../nvme.h"
#include "ftl.h"
#include "nvm_ops.h"

static void *ftl_thread(void *arg);


static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    //assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
        ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

    assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    //QTAILQ_INIT(&lm->victim_line_list);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *custom_wpp = &ssd->custom_wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;
    /* make sure lines are already initialized by now */
    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    /* wpp->curline is always our onging line for writes */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;

    custom_wpp->curline = curline;
    custom_wpp->ch = 0;
    custom_wpp->lun = 0;
    custom_wpp->pg = 0;
    custom_wpp->blk = 0;
    custom_wpp->pl = 0;

/*    filled_wpp->curline = curline;
    filled_wpp->ch = 0;
    filled_wpp->lun = 0;
    filled_wpp->pg = 0;
    filled_wpp->blk = 0;
    filled_wpp->pl = 0;
*/
    ssd->filled_wp_set = false;
}

static void ssd_init_gc_pointer(struct ssd *ssd) {
    struct write_pointer *gcp = &ssd->gc_pointer;

    gcp->curline = NULL;
    
    gcp->ch = 0;
    gcp->lun = 0;
    gcp->pg = 0;
    gcp->blk = 0;
    gcp->pl = 0;

}

static inline void check_addr(int a, int max)
{
    assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        printf("FEMU-FTL: Error, there is no free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    //QTAILQ_INSERT_TAIL(&lm->victim_line_list, wpp->curline, entry);
                    lm->victim_line_cnt++;

                    lm->full_line_cnt++;
                } else {
                    assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    //printf("Coperd,curline,vpc:%d,ipc:%d\n", wpp->curline->vpc, wpp->curline->ipc);
                    assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    //QTAILQ_INSERT_TAIL(&lm->victim_line_list, wpp->curline, entry);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                /* TODO: how should we choose the next block for writes */
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                assert(wpp->pg == 0);
                assert(wpp->lun == 0);
                assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                assert(wpp->pl == 0);
            }
        }
    }
    //printf("Next,ch:%d,lun:%d,blk:%d,pg:%d\n", wpp->ch, wpp->lun, wpp->blk, wpp->pg);
}


static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    assert(ppa.g.pl == 0);

    return ppa;
}

static struct ppa get_custom_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->custom_wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //assert(is_power_of_2(spp->luns_per_ch));
    //assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 4096; /* 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 4;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    int i;

    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    int i;

    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->stale = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    int i;

    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    int i;

    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int i;

    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void fde_init(struct ssd *ssd) {
    uint8_t key1[] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                       0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
    uint8_t key2[] = { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                       0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 };

    memcpy(ssd->fde_meta.key1, key1, KEY_LEN);
    memcpy(ssd->fde_meta.key2, key2, KEY_LEN);

    AES_set_encrypt_key(ssd->fde_meta.key1, KEY_LEN / 2 * 8, &(ssd->fde_meta.aesdata.enc));
    AES_set_encrypt_key(ssd->fde_meta.key1, KEY_LEN / 2 * 8, &(ssd->fde_meta.aesdata.dec));
    AES_set_encrypt_key(ssd->fde_meta.key2, KEY_LEN / 2 * 8, &(ssd->fde_meta.aestweak.enc));
    AES_set_encrypt_key(ssd->fde_meta.key2, KEY_LEN / 2 * 8, &(ssd->fde_meta.aestweak.dec));

    ssd->tv_map =  g_malloc(sizeof(struct tv) * ssd->sp.tt_pgs);
    for(int i = 0; i < ssd->sp.tt_pgs; i++) {
        // We default the IV to the doubled lba
        uint64_t product = i * 4096;
        memset(ssd->tv_map[i].tv, &product, IV_LEN/2);
        memset(ssd->tv_map[i].tv + (IV_LEN/2), &product, IV_LEN/2);
        ssd->tv_map[i].tv[IV_LEN] = 0;
    }

    for(int i = 0; i < 4096/IV_LEN; i++) {
        ssd->ini_pi[i] = i;
    }

    for(int i = 0; i < 4096/IV_LEN; i++) {
        ssd->ini_pi_[ssd->ini_pi[i]] = i;
    }


    mpz_init(ssd->gmp_ds.result);
    mpz_init(ssd->gmp_ds.op1);
    mpz_init(ssd->gmp_ds.op2);
    mpz_init(ssd->gmp_ds.value);
    mpz_init(ssd->gmp_ds.res1);
    mpz_init(ssd->gmp_ds.res2);
    mpz_init(ssd->gmp_ds.res3);
    mpz_init(ssd->gmp_ds.r_result);




    gmp_randinit_default(ssd->gmp_ds.state);

    mpz_urandomb(ssd->gmp_ds.value, ssd->gmp_ds.state, ssd->batch_size * 8);


}

void ssd_init(FemuCtrl *n)
{
    int i;
    struct ssd *ssd = &n->ssd;
    struct ssdparams *spp = &ssd->sp;

    assert(ssd);

    ssd_init_params(spp);
    
    ssd->crypto_buf = g_malloc(sizeof(uint8_t) * spp->secsz * spp->secs_per_pg);
    //ssd->iv_buf = g_malloc(sizeof(uint8_t) * IV_LEN);

    ssd->tt_lat_fde = 0;
    ssd->num_fde = 0;
    ssd->tt_lat_ftl = 0;
    ssd->tt_flash_lat = 0;
    ssd->tt_perm_lat = 0;
    ssd->num_ftl = 0;
    ssd->num_perm = 0;
    ssd->copied_pg_num = 0;

    ssd->stale_pg_num = 0;
    ssd->pg_in_flash = 0;
    ssd->total_pg = 0;
    ssd->hit_in_nvm = 0;
    ssd->total_w_pg = 0;
    ssd->io_num = 0;
    ssd->total_time = 0;
    ssd->ow_cache_pg_num = 0;

    gmp_randinit_mt(ssd->gmp_state);

    ssd->fde_mode = n->fde;

    if(IV_LEN == 16) {
        ssd->batch_size = 210 - 6; // Total hidden size minus metadata size

    } else if (IV_LEN == 32) {
        ssd->batch_size = 89 - 6; // Total hidden size minus metadata size

    } else
        assert(0);

    // Default pd mode is 0, the public mode.
    ssd->pd_mode = 0;

    ssd->erase_num = 0;

    ssd->flash_filled = false;
    ssd->erased_list_set = g_malloc0(sizeof(struct erased_blk_list_per_lun) *
        spp->luns_per_ch * spp->nchs);
    for(i = 0; i < spp->nchs * spp->luns_per_ch; i++) {
      ssd->erased_list_set[i].blk_num = 0;
    }

    ssd->current_w_lun = 0;

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    /* initialize NVM device */
    nvm_init(ssd);

    /* initialize FDE */
    fde_init(ssd);

    ssd_init_gc_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "ftl_thread", ftl_thread, n, QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch &&
            pl >= 0 && pl < spp->pls_per_lun && blk >= 0 &&
            blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk &&
            sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa,
        struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *ch = get_ch(ssd, ppa);
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */

        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        printf("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;
    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        //QTAILQ_INSERT_TAIL(&lm->victim_line_list, line, entry);
        lm->victim_line_cnt++;
    }
}

/* update SSD status about one page from PG_FREE -> PG_VALID */
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    assert(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
    line->vpc++;
}

/* only for erase, reset one block to free state */
static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;
    int i;

    for (i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

/* assume the read data will staged in DRAM and then flushed back to NAND */
static uint64_t gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t lat = 0;
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        lat = ssd_advance_status(ssd, ppa, &gcr);
    }
    return lat;
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    //struct ssd_channel *new_ch;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    uint64_t lat;
    /* first read out current mapping info */
    //set_rmap(ssd, lpn, new_ppa);

    //assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    //mark_page_invalid(ssd, old_ppa);
    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        lat = ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
    //new_ch = get_ch(ssd, &new_ppa);
    //new_ch->gc_endtime = new_ch->next_ch_avail_time;

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return lat;
}

/* TODO: now O(n) list traversing, optimize it later */
static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;
    //int max_ipc = 0;
    //int cnt = 0;

#if 0
    if (QTAILQ_EMPTY(&lm->victim_line_list)) {
        return NULL;
    }

    QTAILQ_FOREACH(line, &lm->victim_line_list, entry) {
        //printf("Coperd,%s,victim_line_list[%d],ipc=%d,vpc=%d\n", __func__, ++cnt, line->ipc, line->vpc);
        if (line->ipc > max_ipc) {
            victim_line = line;
            max_ipc = line->ipc;
        }
    }
#endif

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        //printf("Coperd,select a victim line: ipc=%d (< 1/8)\n", victim_line->ipc);
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    //QTAILQ_REMOVE(&lm->victim_line_list, victim_line, entry);
    lm->victim_line_cnt--;
    //printf("Coperd,%s,victim_line_list,chooose-victim-block,id=%d,ipc=%d,vpc=%d\n", __func__, victim_line->id, victim_line->ipc, victim_line->vpc);

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;

    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            ssd->copied_pg_num += 1;
            cnt++;
        } else {
          ssd->stale_pg_num -= 1;
        }
    }

    assert(blk->vpc == cnt);
    /* do we do "erase" here? */
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
    //printf("Coperd,%s,one more free line,free_line_cnt=%d\n", __func__, lm->free_line_cnt);
}

static void swap(uint8_t *pi, uint16_t idx0, uint16_t idx1) {
    uint8_t mid = pi[idx0];

    pi[idx0] = pi[idx1];
    pi[idx1] = mid;

}

static void unrank(struct ssd *ssd, uint32_t n, uint8_t *pi, mpz_t *value) {
    if(n > 0) {
        //swap(pi[n-1], pi[value mod n])
        swap(pi, n-1, mpz_mod_ui(ssd->gmp_ds.result, *value, n));
        mpz_cdiv_q_ui(ssd->gmp_ds.result, *value, n);
        unrank(ssd, n-1, pi, ssd->gmp_ds.result);
    }
    //return pi;
} 

static void rank(struct ssd *ssd, uint32_t n, uint8_t *pi, uint8_t *pi_, mpz_t *result, mpz_t *res1, mpz_t *res2, mpz_t *res3) {

    if(n == 1)
        mpz_init_set(*result, *res1);
    else {
        uint8_t mid = pi[n-1];
        swap(pi, n-1, pi_[n-1]);
        swap(pi_, mid, n-1);
        mpz_add_ui(*res1, *res1, mid);
        rank(ssd, n-1, pi, pi_, res3, res1, res2, res3);
        mpz_mul_ui(*res2, *res3, n);
        mpz_add(*res1, *res1, *res2);
        //mpz_init_set(*result, res1);
    }
}

static void hidden_read(struct ssd *ssd, uint32_t n, uint8_t *pi, uint8_t *pi_) {
    mpz_t res1, res2, res3, result;
    mpz_init(res1);
    mpz_init(res2);
    mpz_init(res3);
    mpz_init(result);

    rank(ssd, n, pi, pi_, &result, &res1, &res2, &res3);
}
/*
static void rank(struct ssd *ssd, uint32_t n, uint8_t *pi, uint8_t *pi_, mpz_t *result) {
    mpz_t res1, res2, res3;
    mpz_init(res1);
    mpz_init(res2);
    mpz_init(res3);

    if(n == 1)
        mpz_init_set(*result, res1);
    else {
        uint8_t mid = pi[n-1];
        swap(pi, n-1, pi_[n-1]);
        swap(pi_, mid, n-1);
        mpz_add_ui(res1, res1, mid);
        rank(ssd, n-1, pi, pi_, &res3);
        mpz_mul_ui(res2, res3, n);
        mpz_add(res1, res1, res2);
        mpz_init_set(*result, res1);
    }
}

static void hidden_read(struct ssd *ssd, uint32_t n, uint8_t *pi, uint8_t *pi_) {
    mpz_t result;
    mpz_init(result);
    rank(ssd, n, pi, pi_, &result);
}
*/
static uint64_t hidden_write(struct ssd *ssd, uint64_t batch_id, bool force, mpz_t *value)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *chp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    uint64_t lat = 0;
    bool pg_processed = 0;

    if(ssd->gc_pointer.curline == NULL) {
        victim_line = select_victim_line(ssd, force);
        if (!victim_line) {
            ///////printf("FEMU-FTL: failed to get a victim line!\n");
            abort();
            //return -1;
        }
        ssd->gc_pointer.curline = victim_line;
        ssd->gc_pointer.ch = 0;
        ssd->gc_pointer.lun = 0;
        ssd->gc_pointer.pg = 0;
        ssd->gc_pointer.pl = 0;
        ssd->gc_pointer.blk = victim_line->id;
    }

    ppa.g.blk = ssd->gc_pointer.blk;


// TODO: should skip the copy of invalid flash page.

    ppa.g.ch = ssd->gc_pointer.ch;
    ppa.g.lun = ssd->gc_pointer.lun;
    ppa.g.pl = ssd->gc_pointer.pl; 
    ppa.g.pg = ssd->gc_pointer.pg;

    lat += gc_read_page(ssd, &ppa);
 
    // Permutation operations 
    //mpz_init(ssd->gmp_ds.result);

    uint64_t start_time = 0, end_time = 0;
    start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    unrank(ssd, 4096/IV_LEN, ssd->ini_pi, value);
    end_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    ssd->tt_perm_lat += (end_time - start_time);

    ssd->num_perm += 1;

    lat += gc_write_page(ssd, &ppa);
    ssd->copied_pg_num += 1;

    if(ssd->gc_pointer.ch + 1 < spp->nchs) {
        ssd->gc_pointer.ch += 1;
    } else {
        ssd->gc_pointer.ch = 0;
       
        ssd->gc_pointer.lun += 1;
        if(ssd->gc_pointer.lun >= spp->luns_per_ch) {
            ssd->gc_pointer.lun = 0;
            ssd->gc_pointer.pg += 1;
            if(ssd->gc_pointer.pg >= spp->pgs_per_blk) {
                for(int ch = 0; ch < spp->nchs; ch++) {
                    for(int lun = 0; lun < spp->luns_per_ch; lun++) {
                        ppa.g.ch = ch;
                        ppa.g.lun = lun;

                        /* update line status */
                        lunp = get_lun(ssd, &ppa);
                        mark_block_free(ssd, &ppa);
 
                        if (spp->enable_gc_delay) {
                            struct nand_cmd gce;
                            gce.type = GC_IO;
                            gce.cmd = NAND_ERASE;
                            gce.stime = 0;
                            ssd_advance_status(ssd, &ppa, &gce);
                        }
                        //chp->gc_endtime = chp->next_ch_avail_time;
                        //lunp->gc_endtime = lunp->next_lun_avail_time;

                    }
                }
                mark_line_free(ssd, &ppa);

                ssd->gc_pointer.pg = 0;
                ssd->gc_pointer.curline = NULL;
            }
        }
    }

            //chp = get_ch(ssd, &ppa);
            //


    return lat;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *chp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        ///////printf("FEMU-FTL: failed to get a victim line!\n");
        //abort();
        return -1;
    }

    ppa.g.blk = victim_line->id;
    //printf("Coperd,%s,FTL,GCing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n",
      //      ssd->ssdname, ppa.g.blk, victim_line->ipc, ssd->lm.victim_line_cnt,
        //    ssd->lm.full_line_cnt, ssd->lm.free_line_cnt);
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            //chp = get_ch(ssd, &ppa);
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            //chp->gc_endtime = chp->next_ch_avail_time;
            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static inline void erase_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn].ppa = UNMAPPED_PPA;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = &n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    uint8_t dev_flag;
    struct ppa ppa;
    uint64_t start_time, end_time;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    while (1) {
        for (int i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }
            assert(req);

            //dev_flag = device_selection(ssd, req->is_write, req);
            //printf("%"PRIu8"\n", dev_flag);
            ssd->num_ftl += 1;
            start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            switch (req->is_write) {
                case 1:
                  lat = nvm_flash_write(ssd, req);
                  break;
                case 0:
                    lat = nvm_flash_read(ssd, req);
                  break;
                default:
                    printf("FEMU: FTL received unkown request type, ERROR\n");
            }
            end_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            ssd->tt_lat_ftl += (end_time - start_time);
            ssd->tt_flash_lat += lat;

            //lat = 0;
            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }
}

/* accept NVMe cmd as input, in order to support more command types in future */
uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    /* TODO: reads need to go through caching layer first */
    /* ... */


    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba; /* sector addr */
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */

    if (end_lpn >= spp->tt_pgs) {
        printf("RD-ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }

    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

    req->gcrt = 0;
#define NVME_CMD_GCT (911)
    if (req->tifa_cmd_flag == NVME_CMD_GCT) {
        /* fastfail IO path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }

            //ch = get_ch(ssd, &ppa);
            lun = get_lun(ssd, &ppa);
            if (req->stime < lun->gc_endtime) {
                in_gc = true;
                int tgcrt = lun->gc_endtime - req->stime;
                if (req->gcrt < tgcrt) {
                    req->gcrt = tgcrt;
                }
            } else {
                /* NoGC under fastfail path */
                struct nand_cmd srd;
                srd.type = USER_IO;
                srd.cmd = NAND_READ;
                srd.stime = req->stime;
                sublat = ssd_advance_status(ssd, &ppa, &srd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }
        }

        if (!in_gc) {
            assert(req->gcrt == 0);
            return maxlat;
        }

        assert(req->gcrt > 0);
        if (maxlat > req->gcrt) {
            printf("Coperd,%s,%s,%d,inGC,but qlat(%lu) > gclat(%lu)\n", ssd->ssdname, __func__,
                    __LINE__, maxlat, req->gcrt);
        }
        return 0;
    } else {
        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }

        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
        return maxlat;
    }
}

uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    /* TODO: writes need to go to cache first */
    /* ... */

    if (end_lpn >= spp->tt_pgs) {
        printf("ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }
    //assert(end_lpn < spp->tt_pgs);
    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, len);

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
        //break;
    }

    /* on cache eviction, write to NAND page */

    // are we doing fresh writes ? maptbl[lpn] == FREE, pick a new page
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* overwrite */
            /* update old page information first */
            //printf("Coperd,before-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            mark_page_invalid(ssd, &ppa);
            //printf("Coperd,after-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        /* find a new page */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}


uint8_t device_selection(struct ssd *ssd, uint16_t ops, uint64_t lpn) {
  struct segment_range *write_pointer = ssd -> nvm_write_pointer;
  struct nvm_buffer_range *nvm_buf = ssd->nvm_buffer;

  struct nvm_pg mid_pg = write_pointer->pgs[write_pointer->current_write_point];

  // Write pointer of flash
  struct write_pointer *wpp = &ssd->custom_wp;

  struct ppa ppa = get_custom_new_page(ssd);
  struct nand_lun *lun = get_lun(ssd, &ppa);

  uint64_t avail_time;
  uint64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);


  if(ssd->maptbl_nvm_flash[lpn].dev_flag != 2)
    return ssd->maptbl_nvm_flash[lpn].dev_flag;

  //printf("CH: %"PRIu64", DIE: %"PRIu64"\n", mid_pg->ch, mid_pg->die);
  //avail_time = ((ssd->nvm_ch[mid_pg->ch]).dies[mid_pg->die]).next_die_avail_time;
  avail_time = (ssd->nvm_ch[mid_pg.ch]).next_ch_avail_time;
  
  if(ops == 1) { // write
    //return 2;
    //if(avail_time - current_time > NAND_PROG_LATENCY || (1.0 * nvm_buf->filled_seg_num/nvm_buf->segment_num) > 0.95)
    if(((1.0 * nvm_buf->filled_seg_num/nvm_buf->segment_num) > 0.95 || 
        avail_time + NVM_WRITE_LATENCY > lun->next_lun_avail_time + NAND_PROG_LATENCY)
        & avail_time != 0 & lun->next_lun_avail_time != 0)
      return 2; // Select FLASH
    else
      return 0; //Select NVM
  }

  return 0;
}

/* accept NVMe cmd as input, in order to support more command types in future */
uint64_t flash_read(struct ssd *ssd, NvmeRequest *req)
{
    /* TODO: reads need to go through caching layer first */
    /* ... */


    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba; /* sector addr */
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */

    if (end_lpn >= spp->tt_pgs) {
        printf("RD-ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }

    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

    req->gcrt = 0;
#define NVME_CMD_GCT (911)
    if (req->tifa_cmd_flag == NVME_CMD_GCT) {
        /* fastfail IO path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }

            //ch = get_ch(ssd, &ppa);
            lun = get_lun(ssd, &ppa);
            if (req->stime < lun->gc_endtime) {
                in_gc = true;
                int tgcrt = lun->gc_endtime - req->stime;
                if (req->gcrt < tgcrt) {
                    req->gcrt = tgcrt;
                }
            } else {
                /* NoGC under fastfail path */
                struct nand_cmd srd;
                srd.type = USER_IO;
                srd.cmd = NAND_READ;
                srd.stime = req->stime;
                sublat = ssd_advance_status(ssd, &ppa, &srd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }
        }

        if (!in_gc) {
            assert(req->gcrt == 0);
            return maxlat;
        }

        assert(req->gcrt > 0);
        if (maxlat > req->gcrt) {
            printf("Coperd,%s,%s,%d,inGC,but qlat(%lu) > gclat(%lu)\n", ssd->ssdname, __func__,
                    __LINE__, maxlat, req->gcrt);
        }
        return 0;
    } else {
        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);

            ssd->maptbl_nvm_flash[lpn].r_freq += 1;

            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }

        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
        return maxlat;
    }
}

uint64_t flash_read_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time)
{
    /* TODO: reads need to go through caching layer first */
    /* ... */


    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */
    int batch_num;


    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

#define NVME_CMD_GCT (911)
        /* normal IO read path */

            ppa = get_maptbl_ent(ssd, lpn);

          if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                return 40000;
            }


            ssd->maptbl_nvm_flash[lpn].r_freq += 1;

            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = s_time;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;

    if(ssd->fde_mode == 0 || ssd->pd_mode == 0 || ssd->pd_mode == 1) {
        ;
    } else { // this is for hidden read
        assert(ssd->fde_mode == 1 && ssd->pd_mode == 2);
        batch_num = 4096/ssd->batch_size;

        uint64_t start_time = 0, end_time = 0;

        for(int j = 0; j < batch_num; j++) {

            start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            hidden_read(ssd, 4096/IV_LEN, ssd->ini_pi, ssd->ini_pi_);
            end_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

            ssd->tt_perm_lat += (end_time - start_time);
            ssd->num_perm += 1;

        }
        //printf("hidden! %d %"PRIu64"\n", batch_num, maxlat);

        return (NAND_OOB_READ*batch_num);
    }


        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
        return maxlat;
}

static void ssd_advance_custom_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->custom_wp;
    struct line_mgmt *lm = &ssd->lm;

    
    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
      wpp->ch = 0;
      check_addr(wpp->lun, spp->luns_per_ch);
      wpp->lun++;
      /* in this case, we should go to next lun */
      if (wpp->lun == spp->luns_per_ch) {
          wpp->lun = 0;
          /* go to next page in the block */
          check_addr(wpp->pl, spp->pls_per_lun);
          wpp->pl++;
          if (wpp->pl == spp->pls_per_lun) {
              wpp->pl = 0;

              check_addr(wpp->pg, spp->pgs_per_blk);
              wpp->pg++;
              if(wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->blk++;
                if(wpp->blk == spp->blks_per_pl){
                  wpp->blk = 0;
                  printf("filled!\n");
                  ssd->flash_filled = true;
                }
              }
          }
      }
    }
    //printf("Next,ch:%d,lun:%d,blk:%d,pg:%d\n", wpp->ch, wpp->lun, wpp->blk, wpp->pg);
}

static void update_custom_wp(struct ssd *ssd, struct erased_blk selected_blk) {
  struct write_pointer *custom_wpp = &(ssd->custom_wp);
  
  custom_wpp->ch = selected_blk.ch;
  custom_wpp->lun = selected_blk.lun;
  custom_wpp->pg = 0;
  custom_wpp->blk = selected_blk.blk;
  custom_wpp->pl = selected_blk.pl;

}

static void set_custom_wp(struct ssd *ssd) {
  struct erased_blk_list_per_lun *mid_list = NULL;
  uint64_t count = 0;
  while(1) {
    mid_list = &(ssd->erased_list_set[ssd->current_w_lun]);
    if(mid_list->blk_num > 0) {
      update_custom_wp(ssd, mid_list->blk);
      break;
    }
    ssd->current_w_lun = (ssd->current_w_lun + 1) % (ssd->sp.nchs * ssd->sp.luns_per_ch);
    count++;
    if(count == ssd->sp.nchs * ssd->sp.luns_per_ch) {
      printf("No free space left in flash ssd!\n");
      exit(0);
    }
  }
}

static void ssd_advance_filled_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->custom_wp;
    struct line_mgmt *lm = &ssd->lm;
    struct erased_blk_list_per_lun *mid_next;

    if(ssd->filled_wp_set == false) {
      set_custom_wp(ssd);
      ssd->filled_wp_set = true;
      return;
    }
    check_addr(wpp->pg, spp->pgs_per_blk);
    wpp->pg++;
    if(wpp->pg == spp->pgs_per_blk){
      wpp->pg = 0;
      mid_next = ssd->erased_list_set[ssd->current_w_lun].next;
      if(mid_next == NULL) {
        ssd->erased_list_set[ssd->current_w_lun].blk_num = 0;
        ssd->current_w_lun += 1;
        set_custom_wp(ssd);
      } else {
        ssd->erased_list_set[ssd->current_w_lun].blk_num -= 1;
        ssd->erased_list_set[ssd->current_w_lun].blk = mid_next->blk;
        ssd->erased_list_set[ssd->current_w_lun].next = mid_next->next;
        free(mid_next);
        ssd->current_w_lun += 1;
        set_custom_wp(ssd);
      }
   }
    //printf("Next,ch:%d,lun:%d,blk:%d,pg:%d\n", wpp->ch, wpp->lun, wpp->blk, wpp->pg);
}

uint64_t flash_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    /* TODO: writes need to go to cache first */
    /* ... */

    if (end_lpn >= spp->tt_pgs) {
        printf("ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }
    //assert(end_lpn < spp->tt_pgs);
    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, len);
/*
    while (should_gc_high(ssd)) {
        // perform GC here until !should_gc(ssd) 
        r = do_gc(ssd, true);
        if (r == -1)
            break;
        //break;
    }
*/
    /* on cache eviction, write to NAND page */

    // are we doing fresh writes ? maptbl[lpn] == FREE, pick a new page
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        ssd->maptbl_nvm_flash[lpn].w_freq += 1;
        if (mapped_ppa(&ppa)) {
            /* overwrite */
            /* update old page information first */
            //printf("Coperd,before-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            mark_page_invalid(ssd, &ppa);
            //printf("Coperd,after-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            printf("flash_write: This should not happened!\n");
        }

        /* new write */
        /* find a new page */

          printf("8\n");
        ppa = get_custom_new_page(ssd);

          printf("9\n");

        ssd->maptbl_nvm_flash[lpn].dev_flag = 1;
        ssd->maptbl_nvm_flash[lpn].f_ppa = ppa;

        if(ssd->maptbl_nvm_flash[lpn].n_ppa.ppa != UNMAPPED_PPA) {
          printf("ftl.c: line 1365: nvm ppa error!\n");
        }

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        if(ssd->flash_filled == false) {
          /* need to advance the write pointer here */
          printf("10\n");
          ssd_advance_custom_write_pointer(ssd);
          printf("11\n");
        } else {
          ssd_advance_filled_write_pointer(ssd);
        }
        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

// ppa need to specify ch, lun, pl, and blk
void erase(struct ssd *ssd, struct ppa ppa, uint64_t s_time)
{
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *chp;
    struct nand_lun *lunp;
    struct ppa mid_ppa = ppa;
    int pg;
    uint64_t lpn;
    uint64_t erased_list_id = ppa.g.ch * spp->luns_per_ch + ppa.g.lun;
    
    struct erased_blk_list_per_lun *mid_list = NULL, *list_p;

    if(erased_list_id > spp->nchs * spp->luns_per_ch) {
      printf("erased_list_id error!\n");
      exit(0);
    }

    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
      mid_ppa.g.pg = pg;
      lpn = get_rmap_ent(ssd, &mid_ppa);
    /* first read out current mapping info */
    //set_rmap(ssd, lpn, new_ppa);

      if(lpn == INVALID_LPN)
        continue;
      erase_maptbl_ent(ssd, lpn);
      set_rmap_ent(ssd, INVALID_LPN, &mid_ppa);

      //ssd->maptbl_nvm_flash[lpn].dev_flag = 2;
      //if(ssd->maptbl_nvm_flash[lpn].n_ppa.ppa != UNMAPPED_PPA)
        //printf("ftl.c: Something wrong with NVM ppa!\n");
      ssd->maptbl_nvm_flash[lpn].f_ppa.ppa = UNMAPPED_PPA;
    }
    mark_block_free(ssd, &ppa);

    struct nand_cmd gce;
    gce.type = GC_IO;
    gce.cmd = NAND_ERASE;
    gce.stime = s_time;
    ssd_advance_status(ssd, &ppa, &gce);

    ssd->erased_list_set[erased_list_id].blk_num += 1;
    if(ssd->erased_list_set[erased_list_id].blk_num == 1) {
      ssd->erased_list_set[erased_list_id].blk.ch = ppa.g.ch;
      ssd->erased_list_set[erased_list_id].blk.lun = ppa.g.lun;
      ssd->erased_list_set[erased_list_id].blk.pl = ppa.g.pl;
      ssd->erased_list_set[erased_list_id].blk.blk = ppa.g.blk;

      ssd->erased_list_set[erased_list_id].next = NULL;
    } else {
      mid_list = g_malloc0(sizeof(struct erased_blk_list_per_lun) * 1);
      mid_list->blk_num = ssd->erased_list_set[erased_list_id].blk_num;
      mid_list->next = ssd->erased_list_set[erased_list_id].next;

      mid_list->blk.ch = ppa.g.ch;
      mid_list->blk.lun = ppa.g.lun;
      mid_list->blk.pl = ppa.g.pl;
      mid_list->blk.blk = ppa.g.blk;

      ssd->erased_list_set[erased_list_id].next = mid_list;
    }
    /* update line status */
    //mark_line_free(ssd, &ppa);

    return 0;
}

uint64_t flash_read_erase_blk(struct ssd *ssd, uint64_t lpn, uint64_t s_time)
{

    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa, mid_ppa;
    uint64_t  mid_lpn;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */
    struct nand_cmd srd;

    if(ssd->fde_mode == 1) { // this is for hidden read
        return 0;
    }

    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

        /* normal IO read path */
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                printf("Some thing wrong with stale data!\n");
                return 0;
            }
  
            mid_ppa = ppa;
            for(int i = 0; i < ssd->sp.pgs_per_blk; i++) {
              mid_ppa.g.pg = i;
              mid_lpn = get_rmap_ent(ssd, &mid_ppa);

              if(mid_lpn == INVALID_LPN || mid_lpn == lpn)
                continue;

              srd.type = USER_IO;
              srd.cmd = NAND_READ;
              srd.stime = s_time;
              sublat = ssd_advance_status(ssd, &mid_ppa, &srd);


              //nvm_hot_write_lpn(ssd, mid_lpn, s_time + sublat); // write data to nvm


            }

            erase(ssd, mid_ppa, s_time + sublat);


        
        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
        return maxlat;
}

uint64_t flash_write_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    int batch_num;
    /* TODO: writes need to go to cache first */
    /* ... */

    ssd->pg_in_flash += 1;

    if(ssd->fde_mode == 0 || ssd->pd_mode == 0 || ssd->pd_mode == 1) {
        while (should_gc_high(ssd)) {
            /* perform GC here until !should_gc(ssd) */
            r = do_gc(ssd, true);
            if (r == -1)
                break;
            //break;
        }
    } else { // this is for hidden write
        assert(ssd->fde_mode == 1 && ssd->pd_mode == 2);
        batch_num = 4096/ssd->batch_size;



        for(int i = 0; i < batch_num; i++) {

            curlat += hidden_write(ssd, 0, true, &ssd->gmp_ds.value);

            //maxlat = (curlat > maxlat) ? curlat : maxlat;
        }
        // The hidden_write is supposed to be running in parallel across chips during GC.
        // Current implementation implements it synchronously for simplicity.
        // /32 is because of leveraging the parallelism of flash memory.
        return curlat/32;
    }

//    if(ssd->fde_mode == 1) { // this is for hidden write
//        return 0;
//    }
    // are we doing fresh writes ? maptbl[lpn] == FREE, pick a new page
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {

            //ssd->stale_pg_num += 1;
          mark_page_invalid(ssd, &ppa);
            //printf("Coperd,after-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
          set_rmap_ent(ssd, INVALID_LPN, &ppa);

        }

        /* new write */
        /* find a new page */

        ppa = get_new_page(ssd);

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);


        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = s_time;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

    return maxlat;
}

uint64_t nvm_flash_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    struct nand_block *blk;
    uint8_t dev_flag;
    /* TODO: writes need to go to cache first */
    /* ... */

    if (end_lpn >= spp->tt_pgs) {
        printf("ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }
    //assert(end_lpn < spp->tt_pgs);
    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, len);
/*
    while (should_gc_high(ssd)) {
        // perform GC here until !should_gc(ssd) 
        r = do_gc(ssd, true);
        if (r == -1)
            break;
        //break;
    }
*/
    /* on cache eviction, write to NAND page */

    // are we doing fresh writes ? maptbl[lpn] == FREE, pick a new page
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
      ssd->total_pg += 1;
      ssd->maptbl_nvm_flash[lpn].w_freq += 1;
      ssd->hit_in_nvm += 1;
      ssd->total_w_pg += 1;


      //printf("lpn:%"PRIu64"\n", lpn);
#if 0
      if(ssd->maptbl_nvm_flash[lpn].dev_flag == 0)
        ssd->ow_cache_pg_num += 1;

      if(mapped_ppa(&ssd->maptbl_nvm_flash[lpn].f_ppa)) {

        blk = get_blk(ssd, &ssd->maptbl_nvm_flash[lpn].f_ppa);
        if(blk->stale == 0) {
          ssd->erase_num += 1;
          blk->stale = 1;
        }
        if(ssd->maptbl_nvm_flash[lpn].dev_flag == 1){
          //printf("not hit");
          ssd->hit_in_nvm -= 1;
          ssd->stale_pg_num += 1;
        }

        if(ssd->maptbl_nvm_flash[lpn].f_ppa.is_included_stale == 0) {
          ssd->maptbl_nvm_flash[lpn].f_ppa.is_included_stale = 1;

          //ssd->stale_pg_num += 1;
          ppa = get_maptbl_ent(ssd, lpn);
          assert(mapped_ppa(&ppa));
          ppa.is_included_stale = 1;

          set_maptbl_ent(ssd, lpn, &ppa);
        }
      }
#endif

      curlat = flash_write_lpn(ssd, lpn, req->stime);

//      curlat = nvm_write_lpn(ssd, lpn, req->stime);
      maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    ssd->io_num++;
    ssd->total_time+=maxlat;
    return maxlat;
}

uint64_t nvm_flash_read(struct ssd *ssd, NvmeRequest *req)
{
    /* TODO: reads need to go through caching layer first */
    /* ... */


    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba; /* sector addr */
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */
    uint8_t dev_flag = 0;

    if (end_lpn >= spp->tt_pgs) {
        printf("RD-ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }

    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

    req->gcrt = 0;
#define NVME_CMD_GCT (911)
    if (req->tifa_cmd_flag == NVME_CMD_GCT) {
        /* fastfail IO path */
      return 0;
    } else {
        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            sublat = flash_read_lpn(ssd, lpn, req->stime);

#if 0          
          ssd->total_pg += 1;
          dev_flag = device_selection(ssd, 0, lpn);
          ssd->maptbl_nvm_flash[lpn].r_freq += 1;
          if(dev_flag == 0) {
            ssd->hit_in_nvm += 1;
            sublat = nvm_read_lpn(ssd, lpn, req->stime);
            //lat = 0;
          } else if(dev_flag == 1) {
            sublat = flash_read_lpn(ssd, lpn, req->stime);
            nvm_write_lpn(ssd, lpn, req->stime);

          } else 
            sublat = 0;
#endif
          maxlat = (sublat > maxlat) ? sublat : maxlat;
        }

        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
    ssd->io_num++;
    ssd->total_time+=maxlat;

        return maxlat;
    }
}
