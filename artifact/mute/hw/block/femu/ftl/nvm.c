#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "../nvme.h"
#include "nvm.h"

static void maptbl_nvm_flash_init(struct ssd *ssd) {
  int i;

  ssd -> maptbl_nvm_flash = g_malloc0(sizeof(struct ppa_set) * (ssd->sp).tt_pgs);
  for(i = 0; i < (ssd->sp).tt_pgs; i++) {
    ssd->maptbl_nvm_flash[i].dev_flag = 2;
    ssd->maptbl_nvm_flash[i].n_ppa.ppa = UNMAPPED_PPA;
    ssd->maptbl_nvm_flash[i].f_ppa.ppa = UNMAPPED_PPA;
    ssd->maptbl_nvm_flash[i].r_freq = 0;
    ssd->maptbl_nvm_flash[i].w_freq = 0;


  }
}


static void nvm_buffer_init(struct ssd *ssd) {
  struct nvm_buffer_range *nvm_buffer;
  uint64_t current_ch = 0, current_die = 0, current_pg = 0;
//  uint64_t pg_count = 0;

  ssd->nvm_buffer = g_malloc0(sizeof(struct nvm_buffer_range) * 1);
  nvm_buffer = ssd->nvm_buffer;

  nvm_buffer -> start_add = 0;
  nvm_buffer -> pg_num = ssd->pg_per_seg * ssd->seg_per_buffer;
  nvm_buffer -> segment_num = ssd -> seg_per_buffer;
  nvm_buffer -> filled_seg_num = 0;
  nvm_buffer -> current_allocate_segment_id = 0; //start from the first segment
  nvm_buffer -> current_eviction_seg_id = 0;
  nvm_buffer->evic_seg.seg_id = INVALID_EVIC_SEG;
  nvm_buffer->evic_seg.pg_id = 0;
  nvm_buffer -> segment_set = g_malloc0(sizeof(struct segment_range) * 
      nvm_buffer -> segment_num);
  
  for(int i = 0; i < nvm_buffer -> segment_num; i++) {
    nvm_buffer->segment_set[i].start_add = i * ssd->pg_per_seg * (NVM_PG_SZ_B);
    nvm_buffer->segment_set[i].seg_id = i;
    nvm_buffer->segment_set[i].pg_num = ssd -> pg_per_seg;
    nvm_buffer->segment_set[i].occupied_pg_num = 0;
    nvm_buffer->segment_set[i].hot_pg_num = 0;
    nvm_buffer->segment_set[i].current_write_point = 0;

    nvm_buffer->segment_set[i].pgs = g_malloc0(sizeof(struct nvm_pg) * 
        ssd -> pg_per_seg);

    for(int j = 0; j < ssd -> pg_per_seg; j++) {
      (nvm_buffer->segment_set[i]).pgs[j].status = 0;
      (nvm_buffer->segment_set[i]).pgs[j].ch = current_ch;
      (nvm_buffer->segment_set[i]).pgs[j].die = current_die;
      (nvm_buffer->segment_set[i]).pgs[j].pg = j;
      (nvm_buffer->segment_set[i]).pgs[j].lpn = INVALID_LPN;

//      pg_count++;
      current_ch ++;
      if(current_ch == ssd->nvm_channel_num) {
        current_ch = 0;
        current_die ++;
        if(current_die == ssd->nvm_die_num_per_ch) {
          current_pg ++;
          current_die = 0;
          if(current_pg == ssd->nvm_pgs_num_per_die)
            current_pg = 0;
        }
      }
    }
  }
//  printf("pg_count: %"PRIu64"\n", pg_count);
}

static uint64_t get_allocate_seg_id(struct ssd *ssd) {
  uint64_t allocate_id = ssd->nvm_buffer->current_allocate_segment_id;

  ssd->nvm_buffer->current_allocate_segment_id = 
    (allocate_id + 1) % (ssd->nvm_buffer->segment_num);

  return allocate_id;
}

static struct segment_range *allocate_seg(struct ssd *ssd) {
  struct segment_range *seg_range = &(ssd->nvm_buffer->segment_set[get_allocate_seg_id(ssd)]);


  return seg_range;

}

static void nvm_channel_init(struct ssd *ssd) {
  struct nvm_die *mid_dies;
  ssd -> nvm_ch = g_malloc0(sizeof(struct nvm_channel) * ssd -> nvm_channel_num);

  for(int i = 0; i < ssd -> nvm_channel_num; i++) {
    ssd->nvm_ch[i].ndies = ssd -> nvm_die_num_per_ch;
    ssd->nvm_ch[i].next_ch_avail_time = 0;
    ssd->nvm_ch[i].future_read_time = 0;

    ssd->nvm_ch[i].dies = g_malloc0(sizeof(struct nvm_die) * ssd -> nvm_die_num_per_ch);
    mid_dies = ssd->nvm_ch[i].dies;
    for(int j = 0; j < ssd->nvm_die_num_per_ch; j++) {
      mid_dies[j].next_die_avail_time = 0;
      mid_dies[j].npgs = ssd->nvm_pgs_num_per_die;
    }
  }
}

static struct ppa_set get_maptbl(struct ssd *ssd, uint64_t lpn) {
  return ssd->maptbl_nvm_flash[lpn];
}

static inline bool mapped_ppa(struct ppa *ppa) {
  return !(ppa->ppa == UNMAPPED_PPA);
}

static inline bool mapped_nvm_ppa(struct nvm_ppa *ppa){
  return !(ppa->ppa == UNMAPPED_PPA);
}

static inline bool valid_nvm_ppa(struct ssd *ssd, struct nvm_ppa *ppa) 
{
  int ch = ppa->g.ch;
  int die = ppa->g.die;
  int seg = ppa->g.seg;
  int pg = ppa->g.pg;

  if(ch >=0 && ch < ssd->nvm_channel_num && die >=0 && die < ssd->nvm_die_num_per_ch &&
      seg >= 0 && seg < ssd->nvm_buffer->segment_num && pg >= 0 && pg < ssd->pg_per_seg)
    return true;

  return false;
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

static struct nvm_channel *get_nvm_channel(struct ssd *ssd, struct nvm_ppa *ppa) {
  return &(ssd->nvm_ch[ppa->g.ch]);
}

static struct nvm_die *get_nvm_die(struct ssd *ssd, struct nvm_ppa *ppa) {
  struct nvm_channel *ch = get_nvm_channel(ssd, ppa);
  return &(ch->dies[ppa->g.die]);
}

static uint64_t nvm_get_lat(struct ssd *ssd, struct nvm_ppa *ppa, struct nvm_cmd *ncmd) {
  uint64_t start_time = (ncmd->stime == 0) ? \
    qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
  uint64_t nvm_start_time, die_start_time, ch_read_start_time;;
  struct nvm_die *die = get_nvm_die(ssd, ppa);
  struct nvm_channel *ch = get_nvm_channel(ssd, ppa);

  return 50;
  switch(ncmd->io_type) {
    case NVM_READ:
      nvm_start_time = (start_time > ch->next_ch_avail_time) ? 
        start_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = nvm_start_time + NVM_READ_LATENCY + NVM_BUS_LATENCY;
      return (ch->next_ch_avail_time - start_time);
#if 0
      die_start_time = (start_time > die->next_die_avail_time) ?
        start_time : die->next_die_avail_time;
      
      die->next_die_avail_time = die_start_time + NVM_READ_LATENCY;
      ch_read_start_time = (die->next_die_avail_time > ch->next_ch_avail_time)
        ? die->next_die_avail_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = ch_read_start_time + NVM_BUS_LATENCY;

      return (ch->next_ch_avail_time - start_time);
#endif

    case NVM_WRITE:
      nvm_start_time = (start_time > ch->next_ch_avail_time) ? 
        start_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = nvm_start_time + NVM_WRITE_LATENCY + NVM_BUS_LATENCY;
      return (ch->next_ch_avail_time - start_time);

#if 0
      nvm_start_time = (start_time > ch->next_ch_avail_time) ?
        start_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = nvm_start_time + NVM_BUS_LATENCY;
      
      die_start_time = (ch->next_ch_avail_time > die->next_die_avail_time) ?
        ch->next_ch_avail_time : die->next_die_avail_time;
      
      die->next_die_avail_time = die_start_time + NVM_WRITE_LATENCY;

      return (die->next_die_avail_time - start_time);
#endif
    case NVM_ERASE:
      nvm_start_time = (start_time > ch->next_ch_avail_time) ? 
        start_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = nvm_start_time + NVM_WRITE_LATENCY + NVM_BUS_LATENCY;
      return (ch->next_ch_avail_time - start_time);
#if 0
      nvm_start_time = (start_time > ch->next_ch_avail_time) ?
        start_time : ch->next_ch_avail_time;
      ch->next_ch_avail_time = nvm_start_time + NVM_BUS_LATENCY;
      
      die_start_time = (ch->next_ch_avail_time > die->next_die_avail_time) ?
        ch->next_ch_avail_time : die->next_die_avail_time;
      
      die->next_die_avail_time = die_start_time + NVM_ERASE_LATENCY;

      return (die->next_die_avail_time - start_time);
#endif

    default:
      printf("This NVM command is not supported!\n");
      return 0;
  }
}

static struct nvm_ppa get_new_nvm_pg(struct ssd *ssd, uint64_t lpn) {
  struct segment_range *nvm_wp = ssd->nvm_write_pointer;
  uint64_t count = 0;
  struct nvm_ppa n_ppa;
  struct nvm_pg *n_pg, *mid_n_pg;

  assert(lpn != INVALID_LPN);
  
  assert(nvm_wp->occupied_pg_num < nvm_wp->pg_num);

  assert(nvm_wp->current_write_point < nvm_wp->pg_num);
  n_pg = &(nvm_wp->pgs[nvm_wp->current_write_point]);
  nvm_wp->occupied_pg_num += 1;
  nvm_wp->current_write_point += 1;

  while(1) {
    mid_n_pg = &(nvm_wp->pgs[nvm_wp->current_write_point]);
    if(mid_n_pg->status != 0) {
      nvm_wp->current_write_point += 1;
    } else {
      break;
    }
  }

  if(nvm_wp->occupied_pg_num >= nvm_wp->pg_num)
    ssd->nvm_buffer->filled_seg_num += 1;

  n_ppa.g.ch = n_pg->ch;
  n_ppa.g.die = n_pg->die;
  n_ppa.g.seg = nvm_wp->seg_id;
  n_ppa.g.pg = n_pg->pg;

  assert(n_pg->status == 0);
  //printf("ch: %"PRIu64"\t, die: %"PRIu64"\t, pg: %"PRIu64"\t, seg: %"PRIu64"\t\n", n_ppa.g.ch, n_ppa.g.die, n_ppa.g.pg, n_ppa.g.seg);

  n_pg->status = 1;
  //assert(n_pg->lpn == INVALID_LPN);
  n_pg->lpn = lpn;

  while(nvm_wp->occupied_pg_num >= nvm_wp->pg_num) {
    ssd->nvm_write_pointer = allocate_seg(ssd);
    nvm_wp = ssd->nvm_write_pointer;
    count++;
    if(count == ssd->nvm_buffer->segment_num) {
      printf("No free page available!\n");
      break;
    }
  }
  
  return n_ppa;

}

static struct nvm_ppa get_new_nvm_hot_pg(struct ssd *ssd, uint64_t lpn) {
  struct segment_range *nvm_wp = ssd->hot_seg;
  uint64_t count = 0;
  struct nvm_ppa n_ppa;
  struct nvm_pg *n_pg;

  assert(lpn != INVALID_LPN);
  
  assert(nvm_wp->occupied_pg_num < nvm_wp->pg_num);

  n_pg = &(nvm_wp->pgs[nvm_wp->current_write_point]);
  nvm_wp->occupied_pg_num += 1;
  nvm_wp->current_write_point += 1;

  if(nvm_wp->occupied_pg_num >= nvm_wp->pg_num)
    ssd->nvm_buffer->filled_seg_num += 1;

  n_ppa.g.ch = n_pg->ch;
  n_ppa.g.die = n_pg->die;
  n_ppa.g.seg = nvm_wp->seg_id;
  n_ppa.g.pg = n_pg->pg;

  //printf("ch: %"PRIu64"\t, die: %"PRIu64"\t, pg: %"PRIu64"\t, seg: %"PRIu64"\t\n", n_ppa.g.ch, n_ppa.g.die, n_ppa.g.pg, n_ppa.g.seg);

  n_pg->status = 1;
  n_pg->lpn = lpn;

  while(nvm_wp->occupied_pg_num >= nvm_wp->pg_num) {
    ssd->hot_seg = allocate_seg(ssd);
    nvm_wp = ssd->hot_seg;
    count++;
    if(count == ssd->nvm_buffer->segment_num) {
      printf("No free page available!\n");
      break;
    }
  }
  
  return n_ppa;

}

static void set_maptbl_nvm_pg(struct ssd *ssd, uint64_t lpn, struct nvm_ppa n_ppa) {
  assert(lpn < ssd->sp.tt_pgs);

  ssd->maptbl_nvm_flash[lpn].dev_flag = 0;
  ssd->maptbl_nvm_flash[lpn].n_ppa = n_ppa;
  ssd->maptbl_nvm_flash[lpn].f_ppa.ppa = UNMAPPED_PPA;
}

void nvm_init(struct ssd *ssd) {
  //ssd -> nvm_size = 1024 * 1024 * 1024;
  ssd -> nvm_size = NVM_SZ_GB * 1024 * 1024 * 1024 * 1024 / 1024;
  //ssd -> nvm_size = NVM_SZ_GB * ssd -> nvm_size;
  ssd -> pg_per_seg = (ssd->sp).pgs_per_blk;
  ssd -> seg_per_buffer = ssd -> nvm_size / (ssd -> pg_per_seg * NVM_PG_SZ_B);
  ssd -> hot_seg = NULL;
  ssd -> nvm_channel_num = NVM_CH_NUM;
  ssd -> nvm_die_num_per_ch = NVM_DIE_NUM_PER_CH;
  ssd -> nvm_pgs_num_per_die = ssd->nvm_size / 
    (ssd->nvm_channel_num * ssd->nvm_die_num_per_ch * NVM_PG_SZ_B);

  //printf("nvm_size: %"PRIu64", pg_per_seg: %"PRIu64", seg_per_buffer: %"PRIu64", nvm_pgs_num_per_die: %"PRIu64"\n", ssd->nvm_size, ssd->pg_per_seg, ssd->seg_per_buffer, ssd->nvm_pgs_num_per_die);

  maptbl_nvm_flash_init(ssd);

  nvm_buffer_init(ssd);

  nvm_channel_init(ssd);

  ssd -> nvm_write_pointer = allocate_seg(ssd);

}



uint64_t nvm_read(struct ssd *ssd, NvmeRequest *req) {
  struct ppa_set ppa;
  uint64_t lba = req -> slba;
  int len = req->nlb;
  uint64_t start_lpn = lba / (ssd->sp).secs_per_pg;
  uint64_t end_lpn = (lba + len) / (ssd->sp).secs_per_pg;
  uint64_t lpn;
  struct nvm_cmd cmd;
  uint64_t curlat, maxlat = 0;

  assert(end_lpn < ssd->sp.tt_pgs);

  for(lpn = start_lpn; lpn <= end_lpn; lpn++) {
    ppa = get_maptbl(ssd, lpn);
    ssd->maptbl_nvm_flash[lpn].r_freq += 1;

    if(ppa.dev_flag == 2) { // Data not exists
      continue;
    }else if(ppa.dev_flag == 0) {
      if(!mapped_nvm_ppa(&ppa.n_ppa) || !valid_nvm_ppa(ssd, &ppa.n_ppa)) {
         printf("Line 243, nvm.c: NVM address error!\n");
         continue;
      }
    }else if(ppa.dev_flag == 1) {
      printf("Line 247, nvm.c: This should not happen!\n");
      if(!mapped_ppa(&ppa.f_ppa) || !valid_ppa(ssd, &ppa.f_ppa)) {
        printf("Line 249, nvm.c: FLASH address error!\n");
        continue;
      }
    } else {
      printf("Line 253, nvm.c: This should not happen! %"PRIu16"\n", ppa.dev_flag);
      continue;
    }
    cmd.io_type = NVM_READ;
    cmd.stime = req->stime;
    curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
    maxlat = (curlat > maxlat) ? curlat : maxlat;
  }
  return maxlat;
}

static void erase_pg(struct nvm_pg *pg) {
  pg->status = 0;
  pg->lpn = INVALID_LPN;
}

uint64_t nvm_write(struct ssd *ssd, NvmeRequest *req) {
  struct ppa_set ppa;
  uint64_t lba = req->slba;
  int len = req->nlb;
  uint64_t start_lpn = lba / (ssd->sp).secs_per_pg;
  uint64_t end_lpn = (lba + len - 1) / (ssd->sp).secs_per_pg;
  uint64_t lpn;
  struct nvm_cmd cmd;
  uint64_t curlat, maxlat = 0;

  assert(end_lpn < ssd->sp.tt_pgs);

  for(lpn = start_lpn; lpn <= end_lpn; lpn++) {
    ppa = get_maptbl(ssd, lpn);
    ssd->maptbl_nvm_flash[lpn].w_freq += 1;

    //if(ppa.n_ppa.ppa != UNMAPPED_PPA && ppa.dev_flag == 1) {
    //  printf("Line 280, nvm.c: Data exists in both NVM and flash, this should not happened!\n");
    //  continue;
    //}

    if(mapped_nvm_ppa(&ppa.n_ppa)) { //data exists in NVM
      if(!valid_nvm_ppa(ssd, &ppa.n_ppa)) {
        printf("NVM ppa error! This should not happened!\n");
        exit(0);
      }
      cmd.io_type = NVM_WRITE;
      cmd.stime = req->stime;
      curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
      maxlat = (curlat > maxlat) ? curlat : maxlat;
    } else { //first-time data
      ppa.n_ppa = get_new_nvm_pg(ssd, lpn);

      set_maptbl_nvm_pg(ssd, lpn, ppa.n_ppa);

      cmd.io_type = NVM_WRITE;
      cmd.stime = req->stime;
      curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
      maxlat = (curlat > maxlat) ? curlat : maxlat;

    }
  }

  return maxlat;
}



/*uint64_t nvm_hot_write_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time) {
  struct ppa_set ppa;
  struct nvm_cmd cmd;
  uint64_t curlat, maxlat = 0, lat = 0;


    ppa = get_maptbl(ssd, lpn);


    if(mapped_nvm_ppa(&ppa.n_ppa)) { //data exists in NVM
      printf("Data exists in NVM! nvm_hot_write_lpn error!\n");
      return;
    } else { //first-time data
      if(ppa.r_freq + ppa.w_freq < HOTNESS_THRES) {

        lat = nvm_write_lpn(ssd, lpn, s_time);

        return maxlat;
      } else {
        ppa.n_ppa = get_new_nvm_hot_pg(ssd, lpn);

        set_maptbl_nvm_pg(ssd, lpn, ppa.n_ppa);

        cmd.io_type = NVM_WRITE;
        cmd.stime = s_time;
        curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
      }

    }

  return maxlat+lat;
}*/

uint64_t nvm_read_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time) {
  struct ppa_set ppa;
  struct nvm_cmd cmd;
  uint64_t curlat, maxlat = 0;


    ppa = get_maptbl(ssd, lpn);

    if(ppa.dev_flag == 2) { // Data not exists
      return 0;
    }else if(ppa.dev_flag == 0) {
      if(!mapped_nvm_ppa(&ppa.n_ppa) || !valid_nvm_ppa(ssd, &ppa.n_ppa)) {
         printf("Line 243, nvm.c: NVM address error!\n");
         return 0;
      }
    }else if(ppa.dev_flag == 1) {
      printf("Line 527, nvm.c: This should not happened!\n");
      if(!mapped_ppa(&ppa.f_ppa) || !valid_ppa(ssd, &ppa.f_ppa)) {
        printf("Line 249, nvm.c: FLASH address error!\n");
        return 0;
      }
    } else {
      printf("Line 253, nvm.c: This should not happened! %"PRIu16"\n", ppa.dev_flag);
      return 0;
    }
    cmd.io_type = NVM_READ;
    cmd.stime = s_time;
    curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
    maxlat = (curlat > maxlat) ? curlat : maxlat;
  return maxlat;
}

uint64_t nvm_erase(struct ssd *ssd, uint64_t seg_id) {
  struct segment_range *seg = &(ssd->nvm_buffer->segment_set[seg_id]);
  uint64_t lpn;
  struct nvm_cmd cmd;

  seg->occupied_pg_num = 0;
  seg->current_write_point = 0;
  seg->hot_pg_num = 0;

  ssd->nvm_buffer->filled_seg_num -= 1;
  
  for(int i = 0; i < ssd->pg_per_seg; i++) {
    lpn = seg->pgs[i].lpn;
    if(lpn == INVALID_LPN) {
      printf("This should not happend!\n");
    } else {
      //cmd.io_type = NVM_READ;
      //cmd.stime = 0;
      //nvm_get_lat(ssd, &(ssd->maptbl_nvm_flash[lpn].n_ppa), &cmd);

      cmd.io_type = NVM_ERASE;
      cmd.stime = 0;
      nvm_get_lat(ssd, &(ssd->maptbl_nvm_flash[lpn].n_ppa), &cmd);

      ssd->maptbl_nvm_flash[lpn].n_ppa.ppa = UNMAPPED_PPA;
    }

    erase_pg(&(seg->pgs[i]));
  }
}

static uint8_t is_hot_segment(struct ssd *ssd, uint64_t seg_id) {
  struct segment_range *seg = &(ssd->nvm_buffer->segment_set[seg_id]);
  struct nvm_pg pg;
  struct ppa_set ppa;
  uint64_t lpn;
  uint16_t hot_num = 0;

  assert(seg->occupied_pg_num == seg->pg_num);

  for(int i = 0; i < seg->pg_num; i++) {
    pg = seg->pgs[i];
    lpn = pg.lpn;

    assert(lpn != INVALID_LPN);

    ppa = get_maptbl(ssd, lpn);

    if(ppa.dev_flag != 0 || ppa.n_ppa.ppa == UNMAPPED_PPA) {
      printf("Something went wrong in maptbl of ppaset!\n");
      exit(0);
    }

    if(ppa.r_freq + ppa.w_freq >= HOTNESS_THRES)
      hot_num++;
  }
  seg->hot_pg_num = hot_num;

  if((1.0 * hot_num) / seg->pg_num >= HOTNESS_RATIO_THRES)
    return 1;
  else
    return 0;

}

uint64_t nvm_eviction(struct ssd *ssd) {
  struct nvm_buffer_range *nvm_buffer = ssd->nvm_buffer;
  struct segment_range *mid_seg;
  uint16_t evicted_seg_num;
  struct nvm_pg *pg;
  uint64_t lpn;
  uint64_t lat;
  uint64_t current_evic_id;

  if((1.0 * nvm_buffer->filled_seg_num)/nvm_buffer->segment_num < EVICTION_THRES)
    return 0;

  /*evicted_seg_num = (nvm_buffer->filled_seg_num * 
    (EVICTION_THRES - EVICTION_FINISH_THRES)) / 
    (ssd->sp.nchs * ssd->sp.luns_per_ch) *
    (ssd->sp.nchs * ssd->sp.luns_per_ch);
*/
  if(nvm_buffer->evic_seg.seg_id == INVALID_EVIC_SEG) {

    //printf("eviction! %"PRIu64"\n", nvm_buffer->current_eviction_seg_id);
    current_evic_id = nvm_buffer->current_eviction_seg_id;
    nvm_buffer->current_eviction_seg_id = (nvm_buffer->current_eviction_seg_id + 1) % nvm_buffer->segment_num;
    nvm_buffer->evic_seg.seg_id = current_evic_id;
    nvm_buffer->evic_seg.pg_id = 0;
    mid_seg = &(nvm_buffer->segment_set[nvm_buffer->evic_seg.seg_id]);
    assert(mid_seg->occupied_pg_num == mid_seg->pg_num);

  }

  mid_seg = &(nvm_buffer->segment_set[nvm_buffer->evic_seg.seg_id]);
  //assert(mid_seg->occupied_pg_num == mid_seg->pg_num);

  for(int i = 0; i < 1 ; i++) {
    if(mid_seg->occupied_pg_num == mid_seg->pg_num) {
      
      mid_seg->current_write_point = 0;
    }

    assert(nvm_buffer->evic_seg.pg_id < mid_seg->pg_num);
    
    pg = &(mid_seg->pgs[nvm_buffer->evic_seg.pg_id]);
        
    lpn = pg->lpn;

    //printf("pg_id:%"PRIu64"\n", nvm_buffer->evic_seg.pg_id);
    assert(lpn != INVALID_LPN);
    assert(pg->status == 1);
          
    lat = nvm_read_lpn(ssd, lpn, 0);
    lat += flash_write_lpn(ssd, lpn, 0);

    pg->status = 0;
    mid_seg->occupied_pg_num -= 1;
    nvm_buffer->evic_seg.pg_id += 1;
    if(nvm_buffer->evic_seg.pg_id == mid_seg->pg_num) {
      nvm_buffer->evic_seg.seg_id = INVALID_EVIC_SEG;
      nvm_buffer->evic_seg.pg_id = 0;
nvm_buffer->filled_seg_num -= 1;
      return lat;
    }
  }
  return lat;
}

uint64_t nvm_write_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time) {
  struct ppa_set ppa;
  struct nvm_cmd cmd;
  uint64_t curlat, maxlat = 0, lat = 0;


    ppa = get_maptbl(ssd, lpn);
    //ssd->maptbl_nvm_flash[lpn].w_freq += 1;

    lat = nvm_eviction(ssd);

    if(mapped_nvm_ppa(&ppa.n_ppa)) { //data exists in NVM

      set_maptbl_nvm_pg(ssd, lpn, ppa.n_ppa);

      cmd.io_type = NVM_WRITE;
      cmd.stime = s_time;
      curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
      maxlat = (curlat > maxlat) ? curlat : maxlat;
    } else { //first-time data
      ppa.n_ppa = get_new_nvm_pg(ssd, lpn);

      set_maptbl_nvm_pg(ssd, lpn, ppa.n_ppa);

      cmd.io_type = NVM_WRITE;
      cmd.stime = s_time;
      curlat = nvm_get_lat(ssd, &(ppa.n_ppa), &cmd);
      maxlat = (curlat > maxlat) ? curlat : maxlat;

    }

  return maxlat + lat;
}
