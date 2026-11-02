#include <stdio.h>
#include <influx.h>

int main(int argc, char **argv) {
    struct utsname u; uname(&u);
    struct sysinfo si; sysinfo(&si);
    printf("%s %s on %s (%s), %s\n", u.sysname, u.release, u.nodename, u.machine, u.version);
    printf("Uptime: %lu.%lu s   Memory: %lu MiB used of %lu MiB   Processes: %u   Threads: %u\n",
           (unsigned long)si.uptime_ms / 1000, (unsigned long)(si.uptime_ms / 100) % 10, (unsigned long)si.used_mem >> 20, (unsigned long)si.total_mem >> 20, si.procs, si.threads);
    struct procinfo p[32]; int n = ps(p, 32);
    printf("\n%5s %5s %8s %4s %-6s %s\n", "PID", "CPU%", "MEM(KB)", "THR", "TYPE", "NAME");
    for (int i = 0; i < n; i++) printf("%5d %5d %8lu %4d %-6s %s\n", p[i].pid, p[i].cpu, (unsigned long)p[i].mem / 1024, p[i].threads, p[i].user ? "user" : "kernel", p[i].name);
    struct blkinfo b[8]; n = lsblk(b, 8);
    if (n > 0) printf("\nBlock devices:\n");
    for (int i = 0; i < n; i++) printf("  %s%-10s %6lu MiB  %-6s %s\n", b[i].partition ? "  " : "", b[i].name, (unsigned long)b[i].bytes >> 20, b[i].fs, b[i].model);
    return 0;
}
