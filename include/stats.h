#ifndef STATS_H
#define STATS_H

#include "esp_err.h"
esp_err_t stats_init(void);

float stats_read_internal_temperature(void);
void stats_read_cpu_usage(float *cpu0_usage, float *cpu1_usage);
void stats_print_system_info(void);

#endif
