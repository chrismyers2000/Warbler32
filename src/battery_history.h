#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rolling 24h log of battery/MPPT percentage + charging state, for the
// Diagnostics tab's history graph. A background task samples whichever
// source is currently present (MPPT takes priority over the INA219 battery
// monitor, matching the merged Battery status field elsewhere) once a
// minute — see BATT_HISTORY_* in config.h. Nothing is recorded while
// neither source is present, so gaps in the log just mean "nothing was
// detected then," not zero.
esp_err_t battery_history_init(void);

typedef struct {
    uint32_t age_sec;  // seconds before "now" this sample was taken (0 = most recent)
    uint8_t  pct;
    bool     charging;
} battery_history_point_t;

// Copies up to max_count samples into out, oldest-first (out[0] has the
// largest age_sec). Returns the number written. Safe to call from any task.
int battery_history_copy(battery_history_point_t *out, int max_count);

#ifdef __cplusplus
}
#endif
