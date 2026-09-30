#include "sleep_app_config.h"
#if !SLEEP_APP_SANDBOX
#include "sleep_app.h"
#include "sleep_app_internal.h"
#include "audio/audio.h"
#include "detection/detection.h"
#include "inference/inference.h"
#include "storage/storage.h"
#include "lcd/lcd.h"
#include "input/input.h"
#include <tm/tmonitor.h>

SleepAppContext sleep_app;
static ID tasks[5];
static UINT start_attempted;

void sleep_app_fault(const char *where, ER error)
{
    sleep_app.failed = 1;
    sleep_app.ready = 0;
    HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
    tm_printf((UB *)"Sleep app %s failed: %d; reset to retry.\n", where, (int)error);
    for (;;) tk_slp_tsk(TMO_FEVR);
}

ER sleep_app_start(void)
{
    if (start_attempted) return E_OBJ;
    start_attempted = 1;
    T_CFLG flag = {.flgatr = TA_TFIFO | TA_WSGL, .iflgptn = 0};
    T_CMTX mutex = {.mtxatr = TA_INHERIT};
    ID *flags[] = {&sleep_app.detection_flag, &sleep_app.inference_flag,
                  &sleep_app.lcd_flag, &sleep_app.lcd_ack_flag, &sleep_app.record_flag};
    T_CTSK config[] = {
        {.tskatr = TA_HLNG | TA_RNG3, .task = dTrig_task, .itskpri = SLEEP_DETECTION_PRIORITY, .stksz = 2048},
        {.tskatr = TA_HLNG | TA_RNG3, .task = input_task, .itskpri = SLEEP_INPUT_PRIORITY, .stksz = 4096},
        {.tskatr = TA_HLNG | TA_RNG3, .task = inference_task, .itskpri = SLEEP_INFERENCE_PRIORITY, .stksz = 2048},
        {.tskatr = TA_HLNG | TA_RNG3, .task = record_task, .itskpri = SLEEP_RECORD_PRIORITY, .stksz = 1024},
        {.tskatr = TA_HLNG | TA_RNG3, .task = lcd_task, .itskpri = SLEEP_LCD_PRIORITY, .stksz = 2048}
    };
    ER err;
    unsigned started = 0;
    for (unsigned i = 0; i < 5U; ++i) {
        *flags[i] = tk_cre_flg(&flag);
        if (*flags[i] < E_OK) { err = (ER)*flags[i]; goto failed; }
    }
    sleep_app.lcd_mutex = tk_cre_mtx(&mutex);
    if (sleep_app.lcd_mutex < E_OK) {
        err = (ER)sleep_app.lcd_mutex;
        goto failed;
    }
    for (unsigned i = 0; i < 5U; ++i) {
        tasks[i] = tk_cre_tsk(&config[i]);
        if (tasks[i] < E_OK) { err = (ER)tasks[i]; goto failed; }
    }
    err = audio_start();
    if (err < E_OK) goto failed;
    for (; started < 5U; ++started) {
        err = tk_sta_tsk(tasks[started], 0);
        if (err < E_OK) goto failed;
    }
    /* DMA受信は設定・Good Night・待機時間の完了後にinput_taskが要求する。 */
    tm_putstring((UB *)"Sleep app ready: SW1=ENTER/STOP, SW2=minute select.\n");
    return E_OK;

failed:
    sleep_app.ready = 0;
    sleep_app.failed = 1;
    for (unsigned i = 0; i < started; ++i) (void)tk_ter_tsk(tasks[i]);
    audio_cancel_start();
    for (unsigned i = 0; i < 5U; ++i) {
        if (tasks[i] > 0) (void)tk_del_tsk(tasks[i]);
        if (*flags[i] > 0) (void)tk_del_flg(*flags[i]);
    }
    if (sleep_app.lcd_mutex > 0) (void)tk_del_mtx(sleep_app.lcd_mutex);
    return err;
}
#endif
