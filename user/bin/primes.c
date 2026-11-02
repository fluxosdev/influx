/* CPU-bound demo: counts primes with trial division so the Task Manager has something to show. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <influx.h>

int main(int argc, char **argv) {
    long limit = argc > 1 ? atoi(argv[1]) : 2000000;
    long start = uptime_ms(), count = 0;
    for (long n = 2; n <= limit; n++) {
        int prime = 1;
        for (long d = 2; d * d <= n; d++) if (n % d == 0) { prime = 0; break; }
        count += prime;
    }
    printf("%ld primes below %ld (computed in %ld ms, pid %d)\n", count, limit, uptime_ms() - start, getpid());
    return 0;
}
