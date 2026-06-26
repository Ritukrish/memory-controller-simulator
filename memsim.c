#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REQS 1000

/* Config structure */
typedef struct {
    int tRC;
    int thit;

    int num_banks;
    int queue_size;

    int row_bits;
    int bank_bits;
    int col_bits;

    int enable_log;
} Config;

/* Request structure */
typedef struct {
    int cycle;
    char op;

    unsigned int addr;
    int data;
    int id;

    int row;
    int bank;
    int col;
    
   
    int issued;
    int completion_cycle;
} Request;

//Bank Status
typedef struct {
    int has_open_row;
    unsigned int open_row;
    int busy_until;
    int active_cycles;
} BankStatus;

//Written data
typedef struct {
    unsigned int addr;
    int data;
} StorageEntry;

StorageEntry memory_storage[MAX_REQS];
int storage_count = 0;

//Write
void write_to_storage(unsigned int addr, int data) {
    for (int i = 0; i < storage_count; i++) {
        if (memory_storage[i].addr == addr) {
            memory_storage[i].data = data;
            return;
        }
    }
    memory_storage[storage_count].addr = addr;
    memory_storage[storage_count].data = data;
    storage_count++;
}

int read_from_storage(unsigned int addr) {
    for (int i = 0; i < storage_count; i++) {
        if (memory_storage[i].addr == addr) {
            return memory_storage[i].data;
        }
    }
    return 0; 
}

/* Parsers */
void readConfig(char *filename, Config *cfg) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Error opening configuration file.\n");
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[50], eq[5];
        int val;
        if (sscanf(line, "%s %s %d", key, eq, &val) >= 3) {
            if (strcmp(key, "tRC") == 0) cfg->tRC = val;
            else if (strcmp(key, "thit") == 0) cfg->thit = val;
            else if (strcmp(key, "num_banks") == 0) cfg->num_banks = val;
            else if (strcmp(key, "queue_size") == 0) cfg->queue_size = val;
            else if (strcmp(key, "row_bits") == 0) cfg->row_bits = val;
            else if (strcmp(key, "bank_bits") == 0) cfg->bank_bits = val;
            else if (strcmp(key, "col_bits") == 0) cfg->col_bits = val;
            else if (strcmp(key, "enable_log") == 0) cfg->enable_log = val;
        }
    }
    fclose(file);
}

int readTrace(char *filename, Request reqs[]) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Error opening trace file.\n");
        exit(1);
    }
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char data_str[50];
        Request r;
        if (sscanf(line, "%d %c %x %s %d", &r.cycle, &r.op, &r.addr, data_str, &r.id) >= 5) {
            if (strcmp(data_str, "-") == 0) r.data = 0;
            else r.data = atoi(data_str);
            r.issued = 0;
            r.completion_cycle = -1;
            reqs[count++] = r;
        }
    }
    fclose(file);
    return count;
}

void decodeAddress(Request *r) {
    //  col bits [5:0], bank bits [7:6], row bits [23:8]
    r->col = r->addr & ((1 << 6) - 1);
    r->bank = (r->addr >> 6) & ((1 << 2) - 1);
    r->row = (r->addr >> 8) & ((1 << 16) - 1);
}

int main(int argc, char *argv[]) {
    char *trace_file = NULL;
    char *config_file = NULL;
    char *output_dir = "./output";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) trace_file = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) config_file = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_dir = argv[++i];
    }

    if (!trace_file || !config_file) {
        printf("Usage: %s -t trace.txt -c config.txt [-o output_dir]\n", argv[0]);
        return 1;
    }

    Config cfg;
    Request reqs[MAX_REQS];
    readConfig(config_file, &cfg);
    int total_requests = readTrace(trace_file, reqs);

    for (int i = 0; i < total_requests; i++) {
        decodeAddress(&reqs[i]);
    }


    char filepath[512];
    sprintf(filepath, "%s/response.txt", output_dir);
    FILE *resp_f = fopen(filepath, "w");
    sprintf(filepath, "%s/stats.txt", output_dir);
    FILE *stats_f = fopen(filepath, "w");
    FILE *log_f = NULL;

    if (cfg.enable_log) {
        sprintf(filepath, "%s/log", output_dir);
        log_f = fopen(filepath, "w");
        fprintf(log_f, "===== Memory Controller Debug Log =====\nConfig:\n");
        fprintf(log_f, "  Policy           : FCFS\n");
        fprintf(log_f, "  tRC              : %d\n", cfg.tRC);
        fprintf(log_f, "  thit             : %d\n", cfg.thit);
        fprintf(log_f, "  Banks            : %d\n\n", cfg.num_banks);
    }

    fprintf(resp_f, "# <cycle> <id> <data>\n\n");

    BankStatus *banks = (BankStatus *)malloc(cfg.num_banks * sizeof(BankStatus));
    for (int i = 0; i < cfg.num_banks; i++) {
        banks[i].has_open_row = 0;
        banks[i].open_row = 0;
        banks[i].busy_until = 0;
        banks[i].active_cycles = 0;
    }

    Request *queue[MAX_REQS];
    int queue_count = 0;

    int current_cycle = 0;
    int trace_index = 0;
    int completed_count = 0;

    // Simulation statistic
    int total_reads = 0, total_writes = 0;
    int comp_reads = 0, comp_writes = 0;
    long long total_latency = 0;
    int max_latency = 0;
    int min_latency = 1e9;
    long long total_queue_len_accum = 0;

    for (int i = 0; i < total_requests; i++) {
        if (reqs[i].op == 'R') total_reads++;
        else total_writes++;
    }

    while (completed_count < total_requests) {
        int log_cycle_printed = 0;

        // Check happenings in this cycle 
        if (cfg.enable_log) {
            int needs_log = 0;
            if (trace_index < total_requests && reqs[trace_index].cycle == current_cycle) needs_log = 1;
            for (int i = 0; i < queue_count; i++) {
                if (queue[i]->issued && queue[i]->completion_cycle == current_cycle) needs_log = 1;
            }
            if (needs_log) {
                fprintf(log_f, "Cycle %d:\n", current_cycle);
                log_cycle_printed = 1;
            }
        }

        // 1.  Completion Stage
        for (int i = 0; i < queue_count; i++) {
            if (queue[i]->issued && queue[i]->completion_cycle == current_cycle) {
                if (queue[i]->op == 'R') {
                    comp_reads++;
                    queue[i]->data = read_from_storage(queue[i]->addr);
                    fprintf(resp_f, "%d    %d    %d\n", current_cycle, queue[i]->id, queue[i]->data);
                    if (cfg.enable_log) fprintf(log_f, "  Complete ID=%d  Data=%d\n", queue[i]->id, queue[i]->data);
                } else {
                    comp_writes++;
                    write_to_storage(queue[i]->addr, queue[i]->data);
                    if (cfg.enable_log) fprintf(log_f, "  Complete ID=%d  WriteData=%d\n", queue[i]->id, queue[i]->data);
                }

                int latency = current_cycle - queue[i]->cycle;
                total_latency += latency;
                if (latency > max_latency) max_latency = latency;
                if (latency < min_latency) min_latency = latency;

                completed_count++;

                
                for (int j = i; j < queue_count - 1; j++) {
                    queue[j] = queue[j + 1];
                }
                queue_count--;
                i--; 
            }
        }

        // 2.  Entry Enqueueing Stage 
        while (trace_index < total_requests && reqs[trace_index].cycle == current_cycle) {
            if (queue_count < cfg.queue_size) {
                queue[queue_count++] = &reqs[trace_index];
                if (cfg.enable_log) {
                    if (!log_cycle_printed) {
                        fprintf(log_f, "Cycle %d:\n", current_cycle);
                        log_cycle_printed = 1;
                    }
                    fprintf(log_f, "  Enqueue  ID=%d  Op=%c  Addr=0x%x  Bank=%d Row=0x%x\n",
                            reqs[trace_index].id, reqs[trace_index].op, reqs[trace_index].addr, reqs[trace_index].bank, reqs[trace_index].row);
                }
                trace_index++;
            } else {
                break; // Stall  when storage  are full
            }
        }

        // 3.  Request Scheduling Policy ( FCFS)
// Allow one issue 

int *bank_issued = (int *)calloc(cfg.num_banks, sizeof(int));

for (int i = 0; i < queue_count; i++) {

    if (queue[i]->issued)
        continue;

    int b = queue[i]->bank;

    /* Already issued to this bank this cycle */
    if (bank_issued[b])
        continue;

    /* Bank still busy */
    if (current_cycle < banks[b].busy_until)
        continue;

    int is_hit =
        banks[b].has_open_row &&
        (banks[b].open_row == queue[i]->row);

    int latency = is_hit ? cfg.thit : cfg.tRC;

    queue[i]->issued = 1;
    queue[i]->completion_cycle = current_cycle + latency;

    banks[b].busy_until = current_cycle + latency;
    banks[b].has_open_row = 1;
    banks[b].open_row = queue[i]->row;
    banks[b].active_cycles += latency;

    bank_issued[b] = 1;

    if (cfg.enable_log) {
        if (!log_cycle_printed) {
            fprintf(log_f, "Cycle %d:\n", current_cycle);
            log_cycle_printed = 1;
        }

        fprintf(log_f,
                "  Issue    ID=%d  Bank=%d  %s   Lat=%d  Complete@%d\n",
                queue[i]->id,
                b,
                is_hit ? "RowHit" : "RowMiss",
                latency,
                queue[i]->completion_cycle);
    }
}

free(bank_issued);
        total_queue_len_accum += queue_count;
        current_cycle++;
    }

    if (cfg.enable_log) {
        fprintf(log_f, "\nSimulation Complete\nTotal Cycles: %d\n", current_cycle-1);
        fclose(log_f);
    }

    // Save outputs
    double avg_latency = (double)total_latency / total_requests;
    double avg_q_len = (double)total_queue_len_accum / (current_cycle - 1);

    fprintf(stats_f, "===== Simulation Statistics =====\n\n");
    fprintf(stats_f, "Total Requests         : %d\n", total_requests);
    fprintf(stats_f, "Reads                  : %d\n", total_reads);
    fprintf(stats_f, "Writes                 : %d\n\n", total_writes);
    fprintf(stats_f, "Completed Reads        : %d\n", comp_reads);
    fprintf(stats_f, "Completed Writes       : %d\n\n", comp_writes);
    fprintf(stats_f, "Average Latency        : %.1f cycles\n", avg_latency);
    fprintf(stats_f, "Max Latency            : %d cycles\n", max_latency);
    fprintf(stats_f, "Min Latency            : %d cycles\n\n", min_latency == 1e9 ? 0 : min_latency);
    fprintf(stats_f, "===== Timing =====\n");
    fprintf(stats_f, "Total Cycles           : %d\n", current_cycle-1);
    fprintf(stats_f, "Average Queue Length   : ~%.1f\n\n", avg_q_len);
    fprintf(stats_f, "===== Bank Utilization =====\n");

    for (int i = 0; i < cfg.num_banks; i++) {
        double util = ((double)banks[i].active_cycles / current_cycle) * 100.0;
        fprintf(stats_f, "Bank %d Utilization    : ~%.0f%%\n", i, util);
    }

    fclose(resp_f);
    fclose(stats_f);
    free(banks);

    return 0;
}