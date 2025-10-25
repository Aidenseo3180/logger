#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

#define OUT_FILE       "cpu_stats.csv"
#define THERMAL_DIR    "/sys/class/thermal"
#define CPU_DIR        "/sys/devices/system/cpu"
#define MAX_CPUS       128
#define MAX_THERMAL_ZONES 128

typedef struct {
    int cpu_num;
    char freq_path[256];
} cpu_info_t;

typedef struct {
    char zone_name[32];
    char type_name[64];
    char temp_path[256];
} thermal_info_t;

static cpu_info_t cpus[MAX_CPUS];
static int num_cpus = 0;

static thermal_info_t thermal_zones[MAX_THERMAL_ZONES];
static int num_thermal_zones = 0;

// ---- fast int parser ----
static int fast_parse_int(const char *s) {
    int neg = 0, seen = 0;
    long v = 0;
    while (*s && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        seen = 1;
        v = v * 10 + (*s - '0');
        s++;
    }
    if (!seen) return -1;
    return neg ? (int)(-v) : (int)v;
}

static int read_int_file(const char *path) {
    char buf[128];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return fast_parse_int(buf);
}

static int read_string_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz-1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    // remove trailing newline
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    return 0;
}

// CPU 정보 스캔
static void scan_cpus() {
    DIR *dir = opendir(CPU_DIR);
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && num_cpus < MAX_CPUS) {
        if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
        if (!isdigit(entry->d_name[3])) continue;
        
        int cpu_num = atoi(entry->d_name + 3);
        
        // cpufreq 디렉토리가 있는지 확인
        char test_path[256];
        snprintf(test_path, sizeof(test_path), "%s/%s/cpufreq/scaling_cur_freq", 
                 CPU_DIR, entry->d_name);
        
        if (access(test_path, R_OK) == 0) {
            cpus[num_cpus].cpu_num = cpu_num;
            snprintf(cpus[num_cpus].freq_path, sizeof(cpus[num_cpus].freq_path),
                     "%s/%s/cpufreq/scaling_cur_freq", CPU_DIR, entry->d_name);
            num_cpus++;
        }
    }
    closedir(dir);
}

// Thermal zone 스캔 (CPU 관련만)
static void scan_thermal_zones() {
    DIR *dir = opendir(THERMAL_DIR);
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && num_thermal_zones < MAX_THERMAL_ZONES) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        
        char type_path[256], type_buf[64];
        snprintf(type_path, sizeof(type_path), "%s/%s/type", THERMAL_DIR, entry->d_name);
        
        if (read_string_file(type_path, type_buf, sizeof(type_buf)) == 0) {
            // CPU 관련 thermal zone만 (cpu, cpuss 등)
            if (strstr(type_buf, "cpu") != NULL || strstr(type_buf, "CPU") != NULL) {
                strcpy(thermal_zones[num_thermal_zones].zone_name, entry->d_name);
                strcpy(thermal_zones[num_thermal_zones].type_name, type_buf);
                snprintf(thermal_zones[num_thermal_zones].temp_path, 
                         sizeof(thermal_zones[num_thermal_zones].temp_path),
                         "%s/%s/temp", THERMAL_DIR, entry->d_name);
                num_thermal_zones++;
            }
        }
    }
    closedir(dir);
}

// ---- usage ----
static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s -t <seconds>\n"
        "   or: %s -time <seconds>\n", p, p);
}

static int parse_time_arg(int argc, char **argv, long *out) {
    for (int i=1; i<argc; ++i) {
        if (strncmp(argv[i], "-time", 5)==0 || strncmp(argv[i], "-t", 2)==0) {
            const char *eq = strchr(argv[i], '=');
            const char *val = eq ? (eq+1) : (i+1<argc ? argv[++i] : NULL);
            if (!val) return -1;
            char *end=NULL;
            long s = strtol(val, &end, 10);
            if (end==val || s<=0) return -1;
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

    // 시스템 스캔
    scan_cpus();
    scan_thermal_zones();

    if (num_cpus == 0) {
        fprintf(stderr, "Error: No CPUs with cpufreq found!\n");
        return 1;
    }

    printf("Found %d CPUs and %d thermal zones\n", num_cpus, num_thermal_zones);

    FILE *out = fopen(OUT_FILE, "w");
    if (!out) {
        fprintf(stderr,"Error: cannot open %s: %s\n", OUT_FILE, strerror(errno));
        return 1;
    }
    static char obuf[1<<20];
    setvbuf(out, obuf, _IOFBF, sizeof(obuf));

    // CSV header 생성
    fprintf(out, "sample");
    
    // CPU frequency columns
    for (int i = 0; i < num_cpus; i++) {
        fprintf(out, ",cpu%d_freq_khz", cpus[i].cpu_num);
    }
    
    // Thermal zone columns
    for (int i = 0; i < num_thermal_zones; i++) {
        fprintf(out, ",%s_mC", thermal_zones[i].type_name);
    }
    
    fprintf(out, "\n");

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (long sample = 1; sample <= seconds; ++sample) {
        fprintf(out, "%ld", sample);
        
        // CPU frequencies
        for (int i = 0; i < num_cpus; i++) {
            int freq = read_int_file(cpus[i].freq_path);
            fprintf(out, ",%d", freq);
        }
        
        // Thermal zones
        for (int i = 0; i < num_thermal_zones; i++) {
            int temp = read_int_file(thermal_zones[i].temp_path);
            fprintf(out, ",%d", temp);
        }
        
        fprintf(out, "\n");

        next.tv_sec += 1;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    fclose(out);
    printf("Data saved to %s\n", OUT_FILE);
    return 0;
}