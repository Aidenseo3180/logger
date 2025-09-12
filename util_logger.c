// util_logger.c
// Logs GPU busy (KGSL) and per-core CPU utilization (cpu0..cpu7) to CSV every second.
//
// Build: cc -O2 -Wall -Wextra -o util_logger util_logger.c
// Usage: ./util_logger [-i interval_sec] [-n samples] [-o output.csv]
//
// Notes:
// - Uses /proc/stat deltas (idle+iowait as idle-like) for CPU utilization.
// - Reads GPU busy from /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage.
// - Prints -1.00 for any CPU core that doesn't exist or when GPU busy isn't available.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define MAX_CORES 8
#define GPU_BUSY_PATH "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"

typedef unsigned long long u64;

typedef struct {
    u64 total[MAX_CORES];
    u64 idle_like[MAX_CORES];
    bool present[MAX_CORES];
} cpu_snap_t;

static void msleep_exact(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int read_gpu_busy(int *out) {
    FILE *fp = fopen(GPU_BUSY_PATH, "r");
    if (!fp) return -1;
    int val = -1;
    if (fscanf(fp, "%d", &val) != 1) { fclose(fp); return -1; }
    fclose(fp);
    if (val < 0) return -1;
    *out = val;
    return 0;
}

// Safe delta (handles wrap, though 64-bit wrap is practically impossible at 1s)
static inline u64 delta_u64(u64 a, u64 b) {
    return (b >= a) ? (b - a) : (b + (UINT64_MAX - a) + 1ULL);
}

static int read_cpu_snapshot(cpu_snap_t *snap) {
    memset(snap, 0, sizeof(*snap));
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        if (!isdigit((unsigned char)line[3])) continue; // skip aggregate line

        unsigned id = 0;
        // Fields (variable length depending on kernel)
        u64 f[10] = {0};
        // Try to parse up to 10 numeric fields after "cpu%u"
        int n = sscanf(line,
                       "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id, &f[0], &f[1], &f[2], &f[3], &f[4],
                       &f[5], &f[6], &f[7], &f[8], &f[9]);
        if (n < 5) continue; // need at least user,nice,system,idle

        if (id < MAX_CORES) {
            u64 total = 0;
            for (int i = 0; i < n-1; i++) total += f[i];
            // idle_like = idle + iowait (if present)
            u64 idle_like = f[3] + ((n >= 6) ? f[4] : 0);

            snap->total[id] = total;
            snap->idle_like[id] = idle_like;
            snap->present[id] = true;
        }
    }
    fclose(fp);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-i interval_sec] [-n samples] [-o output.csv]\n", prog);
}

int main(int argc, char **argv) {
    int interval_sec = 1;     // sampling interval (seconds)
    long samples = -1;        // -1 = infinite
    const char *out_path = "util_log.csv";

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i+1 < argc) {
            interval_sec = atoi(argv[++i]);
            if (interval_sec <= 0) interval_sec = 1;
        } else if (!strcmp(argv[i], "-n") && i+1 < argc) {
            samples = strtol(argv[++i], NULL, 10);
            if (samples < 0) samples = -1;
        } else if (!strcmp(argv[i], "-o") && i+1 < argc) {
            out_path = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "Cannot open output '%s': %s\n", out_path, strerror(errno));
        return 1;
    }

    // CSV header
    fprintf(out, "sample,gpu_busy");
    for (int c = 0; c < MAX_CORES; c++) fprintf(out, ",cpu%d", c);
    fputc('\n', out);
    fflush(out);

    cpu_snap_t s1, s2;
    if (read_cpu_snapshot(&s1) != 0) {
        fprintf(stderr, "Failed to read /proc/stat\n");
        fclose(out);
        return 1;
    }

    long sample_idx = 0;
    while (samples < 0 || sample_idx < samples) {
        msleep_exact(interval_sec * 1000);

        if (read_cpu_snapshot(&s2) != 0) {
            fprintf(stderr, "Failed to read /proc/stat\n");
            break;
        }

        // GPU busy (0..100). If not available, write -1.
        int gpu_busy = -1;
        if (read_gpu_busy(&gpu_busy) != 0) gpu_busy = -1;

        // Compute per-core utilization for cpu0..cpu7
        double util[MAX_CORES];
        for (int c = 0; c < MAX_CORES; c++) {
            if (s1.present[c] && s2.present[c]) {
                u64 dt = delta_u64(s1.total[c], s2.total[c]);
                u64 di = delta_u64(s1.idle_like[c], s2.idle_like[c]);
                if (dt > 0 && di <= dt) {
                    util[c] = 100.0 * (double)(dt - di) / (double)dt;
                } else {
                    util[c] = -1.0;
                }
            } else {
                util[c] = -1.0;
            }
        }

        // Write CSV row
        sample_idx++;
        fprintf(out, "%ld,%d", sample_idx, gpu_busy);
        for (int c = 0; c < MAX_CORES; c++) {
            if (util[c] >= 0.0) fprintf(out, ",%.2f", util[c]);
            else                fprintf(out, ",-1.00");
        }
        fputc('\n', out);
        fflush(out);

        // Prepare for next iteration
        s1 = s2;
    }

    fclose(out);
    return 0;
}
