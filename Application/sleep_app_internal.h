#ifndef SLEEP_APP_INTERNAL_H
#define SLEEP_APP_INTERNAL_H
#include <tk/tkernel.h>
#include <stdint.h>
/* オブジェクトの生成・削除はsleep_app.cが担当。 */
typedef struct {
    ID detection_flag, inference_flag, lcd_flag, lcd_ack_flag, record_flag;
    ID lcd_mutex;
    volatile UINT ready, failed, monitoring;
} SleepAppContext;
extern SleepAppContext sleep_app;
void sleep_app_fault(const char *where, ER error);
#endif
