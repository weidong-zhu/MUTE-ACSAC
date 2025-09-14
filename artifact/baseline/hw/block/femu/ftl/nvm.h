#ifndef __NVM_H
#define __NVM_H

#include "ftl.h"

#define NVM_PG_BITS 32
#define NVM_SEG_BITS 16
#define NVM_DIE_BITS 8
#define NVM_CH_BITS 8

#define NVM_PG_SZ_B 4096
#define NVM_SZ_GB 0.3

#define NVM_DIE_NUM_PER_CH 2
#define NVM_CH_NUM 3

#define EVICTION_THRES 0.85
#define EVICTION_FINISH_THRES 0.85
#define HOTNESS_THRES 3
#define HOTNESS_RATIO_THRES 0.5

#define INVALID_EVIC_SEG 9999999999

enum {
  NVM_READ = 0,
  NVM_WRITE = 1,
  NVM_ERASE = 2,

  NVM_BUS_LATENCY = 5,
  NVM_READ_LATENCY = 4000,
  NVM_WRITE_LATENCY = 5000,
  NVM_ERASE_LATENCY = 5000,
};

struct nvm_ppa {
  union {
    struct {
      uint64_t ch : NVM_CH_BITS;
      uint64_t die : NVM_DIE_BITS;
      uint64_t seg : NVM_SEG_BITS;
      uint64_t pg : NVM_PG_BITS;
    }g;
    uint64_t ppa;
  };
};

struct nvm_pg {
  uint8_t status; // 0 means free, 1 means occupied
  uint64_t ch;
  uint64_t die;
  uint64_t pg;
  uint64_t lpn;
};

struct nvm_cmd {
  uint16_t io_type;
  uint64_t stime;
};

struct selected_evic_seg {
  uint64_t seg_id;
  uint64_t pg_id;
};

struct segment_range {
  uint64_t start_add;
  uint64_t seg_id;
  uint64_t pg_num; //page number in this segment
  uint64_t occupied_pg_num; // used page number
  uint64_t hot_pg_num; // hot page number
  uint64_t current_write_point; //current available pg id
  struct nvm_pg *pgs;
};


struct nvm_buffer_range {
  uint64_t start_add;
  uint64_t pg_num; // pg_num * 4096 = buffer length
  uint64_t segment_num;
  uint64_t filled_seg_num;
  uint64_t current_allocate_segment_id;
  uint64_t current_eviction_seg_id;
  struct selected_evic_seg evic_seg;
  struct segment_range *segment_set; // size = pg_num / pg_per_segment
};

struct nvm_die {
  uint64_t next_die_avail_time;
  uint64_t npgs;
};

struct nvm_channel {
  uint64_t next_ch_avail_time;
  uint64_t future_read_time;
  uint64_t ndies;
  struct nvm_die *dies;
};


//void nvm_init(struct ssd *ssd);
// Judge whether the segments in nvm need to be evicted to flash
//uint8_t is_evicted_from_nvm();

// Select device, 0 means NVM, 1 means FLASH
//uint8_t device_selection(struct ssd *ssd);
#endif
