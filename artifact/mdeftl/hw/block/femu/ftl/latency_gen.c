#include "latency_gen.h"

static inline void PERSISTENT_BARRIER(void)
{
    asm volatile ("sfence\n" : : );
}

#if defined(__i386__)

static inline unsigned long long asm_rdtsc(void)
{
  unsigned long long int x;
  __asm__ volatile (".byte 0x0f, 0x31" : "=A" (x));
  return x;
}

static inline unsigned long long asm_rdtscp(void)
{
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi)::"ecx");
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );

}
#elif defined(__x86_64__)

static inline unsigned long long asm_rdtsc(void)
{
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}

static inline unsigned long long asm_rdtscp(void)
{
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}
#else
#error "Only support for X86 architecture"
#endif


static inline void emulate_latency_with_ns(uint64_t ns) {
  uint64_t start_cycle, end_cycle, cycles;
  
  start_cycle = asm_rdtscp();
  cycles = NS2CYCLE(ns);

  do {
    end_cycle = asm_rdtscp();
  } while(end_cycle - start_cycle < cycles);

}

static void add_delay(int ops, size_t rw_size, struct monitor_session *monitor) { // ops = 0 means write, ops = 1 means read
  uint64_t current_time = 0;
  uint8_t bandwidth_full = 0;
  uint64_t extra_latency = 0;

  current_time = asm_rdtscp();

  if(current_time >= monitor->end_time) {
    monitor->start_time = current_time;
    monitor->end_time = monitor->start_time + NS2CYCLE(MONITOR_TIME_NS);
    monitor->r_bandwidth_occupation = 0;
    monitor->w_bandwidth_occupation = 0;
  }
  if(ops) { //Read
    if(__sync_add_and_fetch(&monitor->r_bandwidth_occupation, rw_size) >= 
        ((NVM_BANDWIDTH_READ << 20) / (SEC_TO_NS(1UL) / MONITOR_TIME_NS))) // Bandwidth full
      bandwidth_full = 1;
    else
      bandwidth_full = 0;
    extra_latency = NVM_LATENCY_READ;
  } else { //Write
    if(__sync_add_and_fetch(&monitor->w_bandwidth_occupation, rw_size) >= 
        ((NVM_BANDWIDTH_WRITE << 20) / (SEC_TO_NS(1UL) / MONITOR_TIME_NS))) // Bandwidth full
      bandwidth_full = 1;
    else
      bandwidth_full = 0;
    extra_latency = NVM_LATENCY_WRITE;
  }

  if(bandwidth_full) {
    if(ops)
      extra_latency += (uint64_t)rw_size *
        (1 - (float)(((float) NVM_BANDWIDTH_READ)/1000) /
        (((float)DRAM_BANDWIDTH_READ)/1000)) / (((float)NVM_BANDWIDTH_READ)/1000);
    else
      extra_latency += (uint64_t)rw_size *
        (1 - (float)(((float) NVM_BANDWIDTH_WRITE)/1000) /
        (((float)DRAM_BANDWIDTH_WRITE)/1000)) / (((float)NVM_BANDWIDTH_WRITE)/1000);
  }
  emulate_latency_with_ns(extra_latency);
}

void nvm_dev_init(char *dev_path, uint8_t **nvm_buf) {
  int fd;

  fd = open(dev_path, O_RDWR);
  if(fd < 0) {
    printf("Failed to open pm device!\n");
    exit(-1);
  }

  *nvm_buf = (uint8_t *) mmap(NULL, DEV_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
  
  if(*nvm_buf == MAP_FAILED) {
    printf("mmap error!\n");
    exit(-1);
  }
}

uint64_t nvm_dax_read(uint8_t *buf, uint64_t add, uint64_t size, uint8_t *nvm_buf, struct monitor_session *monitor) {
  memmove(buf, nvm_buf + add, size);
  
  add_delay(1, size, monitor);
  
  return size;
}

uint64_t nvm_dax_write(uint8_t *buf, uint64_t add, uint64_t size, uint8_t *nvm_buf, struct monitor_session *monitor) {
  pmem_memmove_persist((void *)(nvm_buf + add), buf, size);
  
  PERSISTENT_BARRIER();

  add_delay(0, size, monitor);
  return size;
}
/*
int main() {
  char nvm_path[30] = "/dev/pmem0";
  struct monitor_session monitor;

  uint8_t *nvm_buf;
  uint8_t rw_buf[4096] = {1};

  uint64_t current_cycle, end_cycle;
  monitor.start_time = monitor.end_time = 0;

  dev_init(nvm_path, &nvm_buf);

  printf("Start rw!\n");
  current_cycle = asm_rdtscp();
  
  nvm_write(rw_buf, 8192, 4096, nvm_buf, &monitor);

  end_cycle = asm_rdtscp();

  printf("Write latency is %llu\n", CYCLE2NS(end_cycle - current_cycle));

  current_cycle = asm_rdtscp();
  
  nvm_read(rw_buf, 4096, 4096, nvm_buf, &monitor);

  end_cycle = asm_rdtscp();

  printf("Read latency is %llu\n", CYCLE2NS(end_cycle - current_cycle));

  return 0;
}
*/
