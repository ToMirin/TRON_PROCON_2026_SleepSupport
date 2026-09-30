#ifndef SLEEP_SETTINGS_H
#define SLEEP_SETTINGS_H

#include <stdint.h>

typedef enum {
    SLEEP_TIME_FIELD_HOUR,
    SLEEP_TIME_FIELD_MINUTE
} SleepTimeField;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t delay_minutes;
    uint8_t gain_db;
    SleepTimeField time_field;
} SleepSettings;

#endif
