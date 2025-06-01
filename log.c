#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h> 
#include <signal.h> 
#include <time.h>   

// --- Configuration Constants (can be overridden by CLI args where applicable) ---
const char *DEFAULT_GPU_UTIL_PATH = "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage";
#define CSV_FLUSH_INTERVAL 10 
#define MAX_CUSTOM_SENSORS 50    
#define MAX_SENSOR_PATH_LEN 256  
#define MAX_SENSOR_VALUE_LEN 128 

// --- Structures ---
typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
    unsigned long long guest;       
    unsigned long long guest_nice;  
    int successfully_parsed;
} CpuTime;

// --- Globals ---
volatile sig_atomic_t keep_running = 1;
char **custom_sensor_paths = NULL;
int num_custom_sensors = 0;

// --- Signal Handler ---
void sigint_handler(int signum) {
    (void)signum; 
    keep_running = 0;
}

// --- Function Prototypes ---
void print_usage(const char *prog_name);
int get_gpu_utilization(const char *gpu_path);
int get_cpu_core_times(CpuTime **core_times_array_ptr);
double calculate_core_utilization(const CpuTime *prev_times, const CpuTime *curr_times);
void load_custom_sensor_paths(const char *filename);
int read_generic_sensor_value(const char *path, char *value_buffer, size_t buffer_len);
void free_custom_sensor_paths();


// --- GPU Utilization ---
int get_gpu_utilization(const char *gpu_path) {
    FILE *fp;
    char buffer[32]; 
    int gpu_util = -1;
    fp = fopen(gpu_path, "r");
    if (fp == NULL) { return -1; } // Path not found or not accessible
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer, "%d", &gpu_util) != 1) { gpu_util = -1; } // Parsing failed
    } else { gpu_util = -1; } // Reading failed
    fclose(fp);
    return gpu_util;
}

// --- CPU Utilization ---
int get_cpu_core_times(CpuTime **core_times_array_ptr) {
    FILE *fp;
    char line_buffer[512];
    int num_cores_found = 0;
    CpuTime *temp_core_data = NULL;

    fp = fopen("/proc/stat", "r");
    if (fp == NULL) { perror("Error opening /proc/stat"); return -1; }

    while (fgets(line_buffer, sizeof(line_buffer), fp) != NULL) {
        if (strncmp(line_buffer, "cpu", 3) == 0 && isdigit(line_buffer[3])) {
            num_cores_found++;
        }
    }

    if (num_cores_found == 0) {
        fprintf(stderr, "Error: No CPU core lines (cpu0, cpu1, ...) found in /proc/stat.\n");
        fclose(fp); *core_times_array_ptr = NULL; return 0;
    }

    temp_core_data = (CpuTime *)malloc(num_cores_found * sizeof(CpuTime));
    if (temp_core_data == NULL) {
        perror("Error allocating memory for CPU times");
        fclose(fp); *core_times_array_ptr = NULL; return -1;
    }
    for(int i=0; i<num_cores_found; ++i) {
        memset(&temp_core_data[i], 0, sizeof(CpuTime));
        temp_core_data[i].successfully_parsed = 0;
    }

    rewind(fp); 
    int current_core_index = 0;
    while (fgets(line_buffer, sizeof(line_buffer), fp) != NULL && current_core_index < num_cores_found) {
        if (strncmp(line_buffer, "cpu", 3) == 0 && isdigit(line_buffer[3])) {
            char cpu_label[16]; 
            int values_parsed = sscanf(line_buffer, "%s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   cpu_label,
                   &temp_core_data[current_core_index].user, &temp_core_data[current_core_index].nice,
                   &temp_core_data[current_core_index].system, &temp_core_data[current_core_index].idle,
                   &temp_core_data[current_core_index].iowait, &temp_core_data[current_core_index].irq,
                   &temp_core_data[current_core_index].softirq, &temp_core_data[current_core_index].steal,
                   &temp_core_data[current_core_index].guest, &temp_core_data[current_core_index].guest_nice);
            if (values_parsed >= 1 + 8) { temp_core_data[current_core_index].successfully_parsed = 1; } 
            else { 
                temp_core_data[current_core_index].successfully_parsed = 0; 
            }
            current_core_index++;
        }
    }
    fclose(fp); *core_times_array_ptr = temp_core_data; return num_cores_found; 
}

double calculate_core_utilization(const CpuTime *prev_times, const CpuTime *curr_times) {
    if (!prev_times->successfully_parsed || !curr_times->successfully_parsed) { return -1.0; }
    unsigned long long prev_idle = prev_times->idle + prev_times->iowait;
    unsigned long long curr_idle = curr_times->idle + curr_times->iowait;
    unsigned long long prev_non_idle = prev_times->user + prev_times->nice + prev_times->system + prev_times->irq + prev_times->softirq + prev_times->steal;
    unsigned long long curr_non_idle = curr_times->user + curr_times->nice + curr_times->system + curr_times->irq + curr_times->softirq + curr_times->steal;
    unsigned long long prev_total = prev_idle + prev_non_idle;
    unsigned long long curr_total = curr_idle + curr_non_idle;
    unsigned long long total_diff = curr_total - prev_total;
    unsigned long long idle_diff = curr_idle - prev_idle;
    if (total_diff == 0) return 0.0;
    if (curr_total < prev_total || curr_idle < prev_idle) return 0.0; 
    double util = (double)(total_diff - idle_diff) * 100.0 / (double)total_diff;
    return (util < 0.0) ? 0.0 : (util > 100.0 ? 100.0 : util);
}

// --- Custom Sensor Functions ---
void load_custom_sensor_paths(const char *filename) {
    if (filename == NULL) { // If no filename provided, do nothing
        printf("Info: No sensor list file specified. No custom sensors will be monitored.\n");
        return;
    }
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: Could not open sensor list file '%s'. No custom sensors will be monitored.\n", filename);
        perror(" fopen");
        return;
    }

    char line_buffer[MAX_SENSOR_PATH_LEN];
    int count = 0;
    while (fgets(line_buffer, sizeof(line_buffer), fp) != NULL) {
        if (strcspn(line_buffer, "\r\n") > 0) count++; 
    }

    if (count == 0) { fclose(fp); return; }
    if (count > MAX_CUSTOM_SENSORS) {
        fprintf(stderr, "Warning: Too many sensors in '%s' (%d). Max is %d. Reading first %d.\n", filename, count, MAX_CUSTOM_SENSORS, MAX_CUSTOM_SENSORS);
        count = MAX_CUSTOM_SENSORS;
    }

    custom_sensor_paths = (char **)malloc(count * sizeof(char *));
    if (custom_sensor_paths == NULL) {
        perror("Error allocating memory for sensor paths");
        fclose(fp); return;
    }

    rewind(fp);
    int current_idx = 0;
    while (fgets(line_buffer, sizeof(line_buffer), fp) != NULL && current_idx < count) {
        line_buffer[strcspn(line_buffer, "\r\n")] = 0;
        if (strlen(line_buffer) > 0) { 
            custom_sensor_paths[current_idx] = strdup(line_buffer);
            if (custom_sensor_paths[current_idx] == NULL) {
                perror("Error duplicating sensor path string");
                for (int i = 0; i < current_idx; i++) free(custom_sensor_paths[i]);
                free(custom_sensor_paths); custom_sensor_paths = NULL;
                fclose(fp); return;
            }
            current_idx++;
        }
    }
    num_custom_sensors = current_idx;
    fclose(fp);
    if (num_custom_sensors > 0) {
        printf("Loaded %d custom sensor paths from '%s'.\n", num_custom_sensors, filename);
    }
}

int read_generic_sensor_value(const char *path, char *value_buffer, size_t buffer_len) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { return -1; } 

    if (fgets(value_buffer, buffer_len, fp) != NULL) {
        value_buffer[strcspn(value_buffer, "\r\n")] = 0; 
        fclose(fp);
        return 0; 
    }
    fclose(fp);
    return -1; 
}

void free_custom_sensor_paths() {
    if (custom_sensor_paths != NULL) {
        for (int i = 0; i < num_custom_sensors; ++i) {
            if (custom_sensor_paths[i] != NULL) {
                free(custom_sensor_paths[i]);
            }
        }
        free(custom_sensor_paths);
        custom_sensor_paths = NULL;
        num_custom_sensors = 0;
    }
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o, --out <filename>      Specify output CSV file name (required for CSV output).\n");
    fprintf(stderr, "  -d, --duration <seconds>  Set run duration in seconds.\n");
    fprintf(stderr, "                              (Default: runs indefinitely)\n");
    fprintf(stderr, "  -s, --sensors <filename>  Specify sensor list file name (required for custom sensor monitoring).\n");
    fprintf(stderr, "  -q, --quiet               Disable per-second console output.\n");
    fprintf(stderr, "  -h, --help                Show this help message.\n");
}


// --- Main ---
int main(int argc, char *argv[]) {
    CpuTime *prev_core_stats = NULL;
    CpuTime *curr_core_stats = NULL;
    int num_cpu_cores = 0;
    int prev_num_cpu_cores = 0; 
    int is_first_iteration_console = 1;
    int output_is_tty = isatty(fileno(stdout));
    int gpu_monitoring_available = 0;

    // CLI Configurable Variables
    char *csv_output_filename = NULL; 
    char *sensor_list_filename = NULL; // No default, must be set by --sensors
    long run_duration_seconds = -1; 
    int quiet_mode = 0;


    // --- Argument Parsing ---
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0)) {
            if (i + 1 < argc) { csv_output_filename = argv[++i]; } 
            else { fprintf(stderr, "Error: %s requires a filename.\n", argv[i-1]); print_usage(argv[0]); return 1; }
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0)) {
            if (i + 1 < argc) {
                char *endptr; run_duration_seconds = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || run_duration_seconds <= 0) {
                    fprintf(stderr, "Error: Invalid duration '%s'. Must be a positive integer.\n", argv[i]); print_usage(argv[0]); return 1;
                }
            } else { fprintf(stderr, "Error: %s requires seconds.\n", argv[i-1]); print_usage(argv[0]); return 1; }
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sensors") == 0)) {
            if (i + 1 < argc) { sensor_list_filename = argv[++i]; } 
            else { fprintf(stderr, "Error: %s requires a filename.\n", argv[i-1]); print_usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]); print_usage(argv[0]); return 1;
        }
    }

    FILE *csv_file = NULL;
    unsigned long long csv_seconds_counter = 0;
    unsigned int csv_lines_since_last_flush = 0;
    char **custom_sensor_values = NULL; 

    signal(SIGINT, sigint_handler);

    printf("Initializing CPU, GPU, & Custom Sensor Monitor (C version)...\n");
    if (quiet_mode) { printf("Quiet mode enabled: Per-second console output is disabled.\n"); }
    else { printf("Monitoring all metrics. Press Ctrl+C to stop.\n"); }
    if (run_duration_seconds > 0) { printf("Program will run for %ld seconds.\n", run_duration_seconds); }

    // Load custom sensors only if a file is specified
    if (sensor_list_filename != NULL) {
        load_custom_sensor_paths(sensor_list_filename);
    } else {
        printf("Info: No sensor list file specified via --sensors. No custom sensors will be monitored.\n");
    }


    FILE *temp_gpu_fp = fopen(DEFAULT_GPU_UTIL_PATH, "r");
    if (temp_gpu_fp != NULL) {
        gpu_monitoring_available = 1;
        fclose(temp_gpu_fp);
        printf("GPU utilization monitoring available via %s.\n", DEFAULT_GPU_UTIL_PATH);
    } else {
        printf("Warning: GPU utilization path %s not found. GPU monitoring will show N/A.\n", DEFAULT_GPU_UTIL_PATH);
    }

    num_cpu_cores = get_cpu_core_times(&prev_core_stats);
    if (num_cpu_cores <= 0 || prev_core_stats == NULL) { 
        fprintf(stderr, "Fatal: Could not retrieve initial CPU data. Exiting.\n");
        if (prev_core_stats) free(prev_core_stats);
        free_custom_sensor_paths();
        return 1;
    }
    prev_num_cpu_cores = num_cpu_cores; 
    printf("Found %d CPU core line(s) in /proc/stat.\n", num_cpu_cores);

    if (csv_output_filename != NULL) { 
        csv_file = fopen(csv_output_filename, "w");
        if (csv_file == NULL) {
            perror("Error opening CSV file for writing. CSV logging disabled");
        } else {
            printf("Logging all metrics to %s\n", csv_output_filename);
            fprintf(csv_file, "Timestamp");
            for (int i = 0; i < prev_num_cpu_cores; ++i) { fprintf(csv_file, ",CPU%d", i); } 
            if (gpu_monitoring_available) { fprintf(csv_file, ",GPU_Util"); }
            for (int i = 0; i < num_custom_sensors; ++i) { fprintf(csv_file, ",%s", custom_sensor_paths[i]); }
            fprintf(csv_file, "\n");
            fflush(csv_file);
        }
    } else {
        printf("CSV output disabled as no output file was specified with --out.\n");
    }
    
    if (num_custom_sensors > 0) {
        custom_sensor_values = (char **)malloc(num_custom_sensors * sizeof(char *));
        if (custom_sensor_values != NULL) {
            for (int i = 0; i < num_custom_sensors; ++i) {
                custom_sensor_values[i] = (char *)malloc(MAX_SENSOR_VALUE_LEN);
                if (custom_sensor_values[i] == NULL) { /* Error handled by checking custom_sensor_values[i] later */ }
            }
        } else { perror("Error allocating memory for sensor value pointers"); }
    }

    if (!quiet_mode) printf("\n"); 
    if (num_cpu_cores > 0) sleep(1); 

    while (keep_running) {
        curr_core_stats = NULL; 
        int current_num_cpu_cores = get_cpu_core_times(&curr_core_stats); 
        
        int gpu_util = -1;
        if (gpu_monitoring_available) { gpu_util = get_gpu_utilization(DEFAULT_GPU_UTIL_PATH); }

        if (custom_sensor_values != NULL) {
            for (int i = 0; i < num_custom_sensors; ++i) {
                if (custom_sensor_values[i] != NULL) { 
                    if (read_generic_sensor_value(custom_sensor_paths[i], custom_sensor_values[i], MAX_SENSOR_VALUE_LEN) != 0) {
                        strncpy(custom_sensor_values[i], "N/A", MAX_SENSOR_VALUE_LEN -1);
                        custom_sensor_values[i][MAX_SENSOR_VALUE_LEN-1] = '\0';
                    }
                }
            }
        }

        if (!keep_running) break; 

        if (current_num_cpu_cores <= 0 || curr_core_stats == NULL) {
            if (!quiet_mode) fprintf(stderr, "Warning: Could not retrieve current CPU times. Skipping this interval.\n");
            if (curr_core_stats) free(curr_core_stats); curr_core_stats = NULL; 
            if (keep_running) sleep(1); continue;
        }
        num_cpu_cores = current_num_cpu_cores;


        if (num_cpu_cores != prev_num_cpu_cores && prev_num_cpu_cores != 0 && !quiet_mode) {
             printf("\nNumber of CPU cores changed from %d to %d. Adapting console display.\n\n", prev_num_cpu_cores, num_cpu_cores);
        }
        
        if (!quiet_mode) {
            if (output_is_tty && !is_first_iteration_console) {
                int lines_to_overwrite = 1 + prev_num_cpu_cores + 
                                         (gpu_monitoring_available ? 1 : 0) + 
                                         num_custom_sensors;
                for (int i = 0; i < lines_to_overwrite; ++i) { printf("\033[F\033[K"); }
            }
            
            char time_buffer[30]; time_t now = time(NULL);
            strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
            printf("--- %s ---\n", time_buffer);

            for (int i = 0; i < num_cpu_cores; ++i) { 
                if (i < prev_num_cpu_cores && prev_core_stats[i].successfully_parsed && curr_core_stats[i].successfully_parsed) {
                    double util = calculate_core_utilization(&prev_core_stats[i], &curr_core_stats[i]);
                    printf("Core %2d: %6.2f%%\n", i, util);
                } else if (i < prev_num_cpu_cores) { 
                     printf("Core %2d: Data N/A\n", i);
                } else { 
                     printf("Core %2d: (new) Data N/A\n", i);
                }
            }
            if (gpu_monitoring_available) {
                if (gpu_util != -1) { printf("GPU Util: %3d%%\n", gpu_util); } 
                else { printf("GPU Util: N/A\n"); }
            }
            for (int i = 0; i < num_custom_sensors; ++i) {
                 printf("%s: %s\n", custom_sensor_paths[i], (custom_sensor_values && custom_sensor_values[i]) ? custom_sensor_values[i] : "N/A");
            }
            fflush(stdout); 
        }

        if (csv_file != NULL) { 
            fprintf(csv_file, "%llu", csv_seconds_counter);
            for (int i = 0; i < prev_num_cpu_cores; ++i) { 
                if (i < num_cpu_cores && prev_core_stats[i].successfully_parsed && curr_core_stats[i].successfully_parsed) { 
                    double util = calculate_core_utilization(&prev_core_stats[i], &curr_core_stats[i]);
                    fprintf(csv_file, ",%.2f", util);
                } else { fprintf(csv_file, ",N/A"); }
            }
            if (gpu_monitoring_available) {
                if (gpu_util != -1) { fprintf(csv_file, ",%d", gpu_util); } 
                else { fprintf(csv_file, ",N/A"); }
            }
            for (int i = 0; i < num_custom_sensors; ++i) {
                fprintf(csv_file, ",%s", (custom_sensor_values && custom_sensor_values[i]) ? custom_sensor_values[i] : "N/A");
            }
            fprintf(csv_file, "\n");
            csv_lines_since_last_flush++;
            if (csv_lines_since_last_flush >= CSV_FLUSH_INTERVAL) {
                fflush(csv_file); csv_lines_since_last_flush = 0;
            }
        }
        
        if (prev_core_stats) free(prev_core_stats);
        prev_core_stats = curr_core_stats; curr_core_stats = NULL; 
        
        if (!quiet_mode) {
            prev_num_cpu_cores = num_cpu_cores;
        }
        
        if (!quiet_mode) is_first_iteration_console = 0;
        csv_seconds_counter++;

        if (run_duration_seconds > 0 && (long)csv_seconds_counter >= run_duration_seconds) {
            if (!quiet_mode) printf("\nSpecified duration of %ld seconds reached.\n", run_duration_seconds);
            else printf("Specified duration of %ld seconds reached. Exiting.\n", run_duration_seconds);
            keep_running = 0;
        }

        if (keep_running) { sleep(1); }
    }

    // Cleanup
    if (prev_core_stats) free(prev_core_stats);
    if (curr_core_stats) free(curr_core_stats); 
    if (csv_file != NULL) {
        if (csv_lines_since_last_flush > 0) fflush(csv_file);
        printf("\nClosing CSV file: %s\n", csv_output_filename);
        fclose(csv_file);
    }
    free_custom_sensor_paths();
    if (custom_sensor_values != NULL) {
        for (int i = 0; i < num_custom_sensors; ++i) {
            if (custom_sensor_values[i] != NULL) free(custom_sensor_values[i]);
        }
        free(custom_sensor_values);
    }

    printf("\nMonitoring stopped.\nExiting monitor.\n");
    return 0;
}
