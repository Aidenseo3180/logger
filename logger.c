#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

#define TEMP_PATH "/sys/class/kgsl/kgsl-3d0/temp"                // usually millidegree C
#define FREQ_PATH "/sys/class/kgsl/kgsl-3d0/clock_mhz"           // MHz (int)
#define UTIL_PATH "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage" // 0..100 (int)
#define OUT_FILE  "gpu_stats.csv" 

// Fast integer parser: returns first signed integer found; -1 on no digits.
static int fast_parse_int(const char *s) {
    int neg = 0, seen = 0;
    long v = 0;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        seen = 1;
        v = v * 10 + (*s - '0');
        s++;
    }
    if (!seen) return -1;
    return neg ? (int)(-v) : (int)v;
}   

static int read_int_fd(int fd, char *buf, size_t bufsz) {
    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return fast_parse_int(buf);
}
    
static void usage(const char *p) {
    fprintf(stderr, 
        "Usage: %s -time <seconds> | -t <seconds>\n"
        "       %s -time=<seconds> | -t=<seconds>\n", p, p);
}

static int parse_time_arg(int argc, char **argv, long *out) {
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-time", 5) == 0 || strncmp(argv[i], "-t", 2) == 0) {
            const char *eq = strchr(argv[i], '=');
            const char *val = eq ? (eq + 1) : (i + 1 < argc ? argv[++i] : NULL);
            if (!val) return -1;
            char *end = NULL;
            long s = strtol(val, &end, 10);
            if (end == val || s <= 0) return -1;
            *out = s;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    long seconds = -1;
    if (parse_time_arg(argc, argv, &seconds) != 0) {
        usage(argv[0]);
        return 1;
    }

    // Open sysfs once.
    int fd_temp = open(TEMP_PATH, O_RDONLY | O_CLOEXEC);
    int fd_freq = open(FREQ_PATH, O_RDONLY | O_CLOEXEC);
    int fd_util = open(UTIL_PATH, O_RDONLY | O_CLOEXEC);

    if (fd_temp < 0 || fd_freq < 0 || fd_util < 0) {
        fprintf(stderr, "Error: open sysfs failed (temp=%d, freq=%d, util=%d): %s\n",
                fd_temp, fd_freq, fd_util, strerror(errno));
        // continue anyway; missing fields will be -1
    }

    // Open output; use big stdio buffer to minimize syscalls.
    FILE *out = fopen(OUT_FILE, "w");
    if (!out) {
        fprintf(stderr, "Error: open output '%s' failed: %s\n", OUT_FILE, strerror(errno));
        return 1;
    }
    static char obuf[1 << 20]; // 1 MiB buffer (no malloc at runtime)
    setvbuf(out, obuf, _IOFBF, sizeof(obuf));

    // Header: integers only for speed (temp in millidegree C).
    fprintf(out, "sample,temp_mC,clock_mhz,gpu_busy_pct\n");

    // Prepare 1 Hz absolute timer to reduce drift.
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    char rbuf[128];
    for (long i = 1; i <= seconds; ++i) {
        int t_mC  = read_int_fd(fd_temp, rbuf, sizeof(rbuf));
        int mhz   = read_int_fd(fd_freq, rbuf, sizeof(rbuf));
        int busy  = read_int_fd(fd_util, rbuf, sizeof(rbuf));

        // If any FD read failed once (e.g., node transient), try a one-time reopen.
        if (t_mC < 0 && fd_temp >= 0) { close(fd_temp); fd_temp = open(TEMP_PATH, O_RDONLY | O_CLOEXEC); t_mC = read_int_fd(fd_temp, rbuf, sizeof(rbuf)); }
        if (mhz  < 0 && fd_freq >= 0) { close(fd_freq); fd_freq = open(FREQ_PATH, O_RDONLY | O_CLOEXEC); mhz  = read_int_fd(fd_freq, rbuf, sizeof(rbuf)); }
        if (busy < 0 && fd_util >= 0) { close(fd_util); fd_util = open(UTIL_PATH, O_RDONLY | O_CLOEXEC); busy = read_int_fd(fd_util, rbuf, sizeof(rbuf)); }

        // Write a CSV row. Use integers to avoid float formatting overhead.
        // If a read failed, fields will be -1.
        fprintf(out, "%ld,%d,%d,%d\n", i, t_mC, mhz, busy);

        // Advance to next whole second (absolute sleep).
        next.tv_sec += 1;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    fclose(out);
    if (fd_temp >= 0) close(fd_temp);
    if (fd_freq >= 0) close(fd_freq);
    if (fd_util >= 0) close(fd_util);
    return 0;
}
