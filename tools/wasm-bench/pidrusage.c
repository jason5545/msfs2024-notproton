#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    struct rusage_info_v4 ri;
    int pid = atoi(argv[i]);
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri)) { printf("pid=%d error\n", pid); continue; }
    printf("pid=%d instructions=%llu cycles=%llu user_ns=%llu\n", pid, ri.ri_instructions, ri.ri_cycles, ri.ri_user_time);
  }
  return 0;
}
