#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t parse_size_mb(const char *arg, size_t fallback) {
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (size_t)value;
}

int main(int argc, char *argv[]) {
    const size_t chunk_mb = (argc > 1) ? parse_size_mb(argv[1], 8) : 8;
    const unsigned int sleep_ms = (argc > 2) ? (unsigned int)strtoul(argv[2], NULL, 10) : 1000;
    const size_t chunk_bytes = chunk_mb * 1024U * 1024U;
    int count = 0;

    while (1) {
        char *mem = malloc(chunk_bytes);
        if (!mem) {
            printf("malloc failed after %d allocations\n", count);
            break;
        }
        memset(mem, 'A', chunk_bytes);
        count++;
        printf("allocation=%d chunk=%zuMB total=%zuMB\n",
               count, chunk_mb, (size_t)count * chunk_mb);
        fflush(stdout);
        usleep(sleep_ms * 1000U);
    }
    return 0;
}
