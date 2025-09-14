#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <libpmem.h>

#define NVM_LATENCY_WRITE 0
#define NVM_LATENCY_READ 300

#define NVM_BANDWIDTH_READ 6600
#define NVM_BANDWIDTH_WRITE  3000

#define DRAM_BANDWIDTH_READ 63000
#define DRAM_BANDWIDTH_WRITE 24000

#define MONITOR_TIME_NS 10000

#define SEC_TO_NS(x) (x * 1000000000UL)

#define _CPUFREQ 3500LLU /* MHz */

#define NS2CYCLE(__ns) (((__ns) * _CPUFREQ) / 1000)
#define CYCLE2NS(__cycles) (((__cycles) * 1000) / _CPUFREQ)

#define DEV_SIZE 21474836480UL //20G

struct monitor_session {
  uint64_t start_time;
  uint64_t end_time;
  uint64_t r_bandwidth_occupation;
  uint64_t w_bandwidth_occupation;
};



void nvm_dev_init(char *dev_path, uint8_t **nvm_buf);

uint64_t nvm_dax_read(uint8_t *buf, uint64_t add, uint64_t size, uint8_t *nvm_buf, struct monitor_session *monitor);

uint64_t nvm_dax_write(uint8_t *buf, uint64_t add, uint64_t size, uint8_t *nvm_buf, struct monitor_session *monitor);


