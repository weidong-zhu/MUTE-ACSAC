#ifndef __FTL_H
#define __FTL_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "nvm.h"
#include "fde_crypto.h"
#include <gmp.h>

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))


enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
    NAND_OOB_READ = 20000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

#define SEC_SIZE 512

#define IV_LEN 16

/* Storage side channel for PD mode setup.  */
#define SC_OFF 4096
#define SC_SZ 64

#define HIDDEN_PASSWORD "hiddenpassword"
#define PUBLIC_PASSWORD "publicpassword"

#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* AES-XTS-128 encryption  */
struct TestAES {
    AES_KEY enc;
    AES_KEY dec;
};

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
    uint8_t is_included_stale;
};


typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    uint8_t stale;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct ppa_set {
  uint16_t dev_flag; // 0 means nvm, 1 means flash, 2 means not allocated
  struct nvm_ppa n_ppa;
  struct ppa f_ppa;
  uint64_t r_freq;
  uint64_t w_freq;
};

struct erased_blk {
  int ch;
  int lun;
  int pl;
  int blk;
};

struct erased_blk_list_per_lun {
  struct erased_blk blk;
  uint64_t blk_num;
  struct erased_blk_list_per_lun *next;
};

struct tv {
    uint8_t tv[IV_LEN+1];
};

struct ivs {
    uint8_t ivs[4096/IV_LEN];
};

struct gmp_ds {
    mpz_t result, res1, res2, res3, r_result;
    mpz_t op1;
    mpz_t op2;
    mpz_t value;
    gmp_randstate_t state;
};

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    struct ppa_set *maptbl_nvm_flash;

    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;

    struct write_pointer custom_wp;
    //struct write_pointer filled_wp;

    bool filled_wp_set;

    uint64_t erase_num;

    uint64_t current_w_lun;

    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;

    bool flash_filled;
    struct erased_blk_list_per_lun *erased_list_set;

    /*NVM section*/ 
        uint64_t nvm_size; // with bytes
        uint64_t hit_in_nvm;

    uint64_t nvm_channel_num;
    uint64_t nvm_die_num_per_ch;
    uint64_t nvm_pgs_num_per_die;

    struct nvm_channel *nvm_ch;

    uint64_t pg_per_seg;
    uint64_t seg_per_buffer;
    struct nvm_buffer_range *nvm_buffer;
    
    struct segment_range *nvm_write_pointer;

    struct segment_range *hot_seg;

    uint64_t stale_pg_num;
    uint64_t pg_in_flash;
    uint64_t total_pg;
    uint64_t total_w_pg;
    uint64_t io_num;
    uint64_t total_time;
    uint64_t ow_cache_pg_num;

    uint8_t pd_mode; // 0 is public mode, 1 is hidden mode

    struct crypto_meta fde_meta;

    uint8_t *crypto_buf;
    struct tv *tv_map; // This is tv actually.
    uint8_t fde_mode; // 1 is enabled, 0 is not enabled
    uint8_t ini_pi[(4096/IV_LEN)+1]; // Default ivs' permutation for unrank
    uint8_t ini_pi_[(4096/IV_LEN)+1]; // Default ivs' permutation for unrank

    uint16_t batch_size;
    struct write_pointer gc_pointer;
    struct ivs *ivs_oob; // This requires a significant DRAM for HBT.
    struct gmp_ds gmp_ds;
    gmp_randstate_t gmp_state;


    uint64_t tt_lat_fde;
    uint64_t num_fde;
    uint64_t tt_lat_ftl;
    uint64_t tt_flash_lat;
    uint64_t num_ftl;
    uint64_t tt_perm_lat;
    uint64_t num_perm;

    uint64_t copied_pg_num;

    /*PEARL data structures.*/
    uint8_t *ppa_to_written_num;
    uint8_t *dst_buf[7000];
/*
    // Encoding lookup table for WOM(3,5)
    uint8_t WOM_encode_map[8] = {
        0b00000, // 000 -> 00000
        0b00001, // 001 -> 00001
        0b00100, // 010 -> 00100
        0b01000, // 011 -> 01000
        0b10000, // 100 -> 10000
        0b10100, // 101 -> 10100
        0b11000, // 110 -> 11000
        0b10111  // 111 -> 10111
    };

// Decoding lookup table for WOM(3,5)
    uint8_t WOM_decode_map[32] = {
        0b000, // 00000 -> 000
        0b001, // 00001 -> 001
        0b010, // 00100 -> 010
        0b011, // 01000 -> 011
        0b100, // 10000 -> 100
        0b101, // 10100 -> 101
        0b110, // 11000 -> 110
        0b111, // 10111 -> 111
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF,   // Unused
        0xFF    // Unused
    };
*/


};


#endif
