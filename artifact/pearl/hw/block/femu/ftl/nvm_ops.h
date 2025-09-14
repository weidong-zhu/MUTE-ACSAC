#include "ftl.h"

extern void nvm_init(struct ssd * ssd);
extern uint8_t is_evicted_from_nvm();
extern uint8_t device_selection(struct ssd *ssd, uint16_t ops, uint64_t lpn); // ops = 0 means read, 1 means write
extern uint64_t nvm_read(struct ssd *ssd, NvmeRequest *req);
extern uint64_t nvm_write(struct ssd *ssd, NvmeRequest *req);
extern uint64_t nvm_write_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time);
extern uint64_t nvm_read_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time);
extern uint64_t nvm_erase(struct ssd *ssd, uint64_t seg_id);
extern uint64_t nvm_hot_write_lpn(struct ssd *ssd, uint64_t lpn, uint64_t s_time);
extern uint64_t nvm_eviction(struct ssd *ssd);
