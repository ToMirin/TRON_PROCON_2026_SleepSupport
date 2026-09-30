#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX
#include "input.h"
#include "../audio/audio.h"
#include "../detection/detection.h"
#include "../inference/inference.h"
#include "../lcd/lcd.h"
#include "../storage/storage.h"
#include "../sleep_app_internal.h"
#include "main.h"

typedef enum {
    INPUT_SETTING_TIME,
    INPUT_SETTING_DELAY,
    INPUT_SETTING_GAIN,
    INPUT_WAIT_ENTER,
    INPUT_MONITORING,
    INPUT_SAVE_FAILED
} InputState_t;

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
    GPIO_PinState pressed_level;
    UINT candidate, stable, matches, pressed_edge;
} InputSwitch_t;

typedef struct {
    UINT previous;
    INT progress;
} InputEncoder_t;

extern TIM_HandleTypeDef htim8;

enum { INPUT_ENTER, INPUT_MINUTE, INPUT_SWITCH_COUNT };

static void input_read_switch(InputSwitch_t *sw)
{
    UINT pressed = HAL_GPIO_ReadPin(sw->port, sw->pin) == sw->pressed_level;
    sw->pressed_edge = 0;
    if (pressed != sw->candidate) {
        sw->candidate = pressed;
        sw->matches = 1;
    } else if (sw->matches < 3U) {
        ++sw->matches;
    }
    if (sw->matches == 3U && sw->stable != sw->candidate) {
        sw->stable = sw->candidate;
        sw->pressed_edge = sw->stable;
    }
}

static void input_show(LcdScreen screen)
{
    ER err = lcd_show_and_wait(screen, 0);
    if (err < E_OK) sleep_app_fault("screen transition", err);
}

static void input_show_settings(LcdScreen screen, const SleepSettings *settings)
{
    ER err = lcd_show_settings_and_wait(screen, settings);
    if (err < E_OK) sleep_app_fault("settings screen", err);
}

/* TIM8は16bitで周回する。読み取り間の移動量は32768未満が前提。 */
static INT input_encoder_delta(UINT current, UINT previous)
{
    UINT diff = (current - previous) & 0xffffU;
    return diff < 0x8000U ? (INT)diff : (INT)diff - 65536;
}

static INT input_read_encoder(InputEncoder_t *encoder)
{
    UINT current = __HAL_TIM_GET_COUNTER(&htim8);
    encoder->progress += input_encoder_delta(current, encoder->previous);
    encoder->previous = current;
    INT counter_steps = encoder->progress / SLEEP_ENCODER_COUNTS_STEP;
    encoder->progress -= counter_steps * SLEEP_ENCODER_COUNTS_STEP;
    /* 実機の取り付け方向に合わせ、右回転を値の増加として扱う。 */
    return -counter_steps;
}

static UINT input_wrap(UINT value, INT steps, UINT count)
{
    INT wrapped = (INT)value + steps;
    wrapped %= (INT)count;
    if (wrapped < 0) wrapped += (INT)count;
    return (UINT)wrapped;
}

static void input_adjust_setting(InputState_t state, SleepSettings *settings,
                                 INT steps)
{
    if (steps == 0) return;
    switch (state) {
    case INPUT_SETTING_TIME:
        if (settings->time_field == SLEEP_TIME_FIELD_HOUR)
            settings->hour = (uint8_t)input_wrap(settings->hour, steps, 24U);
        else
            settings->minute = (uint8_t)input_wrap(settings->minute, steps, 60U);
        break;
    case INPUT_SETTING_DELAY:
        settings->delay_minutes = (uint8_t)input_wrap(
            settings->delay_minutes, steps,
            SLEEP_DELAY_MIN_MAX - SLEEP_DELAY_MIN_MIN + 1U);
        break;
    case INPUT_SETTING_GAIN: {
        UINT gain_index = (settings->gain_db - SLEEP_GAIN_MIN_DB) /
                          SLEEP_GAIN_STEP_DB;
        UINT gain_count = (SLEEP_GAIN_MAX_DB - SLEEP_GAIN_MIN_DB) /
                          SLEEP_GAIN_STEP_DB + 1U;
        settings->gain_db = (uint8_t)(SLEEP_GAIN_MIN_DB +
            input_wrap(gain_index, steps, gain_count) * SLEEP_GAIN_STEP_DB);
        break;
    }
    default:
        return;
    }
    input_show_settings(state == INPUT_SETTING_TIME ? LCD_SCREEN_SETTING_TIME :
                        state == INPUT_SETTING_DELAY ? LCD_SCREEN_SETTING_DELAY :
                                                       LCD_SCREEN_SETTING_GAIN,
                        settings);
}

static void input_start_monitoring(const SleepSettings *settings)
{
    ER err = audio_set_gain_db(settings->gain_db);
    if (err < E_OK) sleep_app_fault("audio gain", err);
    input_show(LCD_SCREEN_GOOD_NIGHT);
    tk_dly_tsk(SLEEP_GOOD_NIGHT_MS);
    input_show_settings(LCD_SCREEN_DELAY, settings);
    tk_dly_tsk((RELTIM)settings->delay_minutes * 60U * 1000U);
    detection_reset_session();
    err = inference_reset_session();
    if (err < E_OK) sleep_app_fault("inference reset", err);
    err = storage_begin_night();
    if (err < E_OK) sleep_app_fault("storage begin", err);
    sleep_app.monitoring = 1;
    err = audio_request_start();
    if (err < E_OK) sleep_app_fault("audio start", err);
    input_show(LCD_SCREEN_MONITOR);
}

static ER input_save_night(void)
{
    input_show(LCD_SCREEN_SAVING);
    ER err = storage_save_night();
    if (err == E_OK) {
        input_show(LCD_SCREEN_SAVE_COMPLETE);
        tk_dly_tsk(SLEEP_SAVE_COMPLETE_MS);
    }
    return err;
}

static ER input_finish_monitoring(void)
{
    ER err = storage_end_night();
    if (err < E_OK) return err;
    sleep_app.monitoring = 0;
    err = audio_request_stop();
    if (err < E_OK) return err;
    input_show(LCD_SCREEN_GOOD_MORNING);
    tk_dly_tsk(SLEEP_GOOD_MORNING_MS);

    /* audio_taskの停止と、開始済み推論・記録通知の完了を待つ。 */
    for (unsigned elapsed = 0; elapsed < 2000U; elapsed += SLEEP_INPUT_POLL_MS) {
        AUDIO_STATUS status;
        audio_get_status(&status);
        if (status.state == AUDIO_FAILED) return E_IO;
        if (status.state == AUDIO_STOPPED && !inference_busy() &&
            !storage_busy()) break;
        tk_dly_tsk(SLEEP_INPUT_POLL_MS);
        if (elapsed + SLEEP_INPUT_POLL_MS >= 2000U) return E_TMOUT;
    }
    return input_save_night();
}

void input_task(INT stacd, void *exinf)
{
    InputSwitch_t switches[INPUT_SWITCH_COUNT] = {
        {SW1_USER_GPIO_Port, SW1_USER_Pin, GPIO_PIN_SET, 0, 0, 0, 0},
        {SW2_GPIO_Port, SW2_Pin, GPIO_PIN_RESET, 0, 0, 0, 0}
    };
    InputState_t state = INPUT_SETTING_TIME;
    SleepSettings settings = {
        SLEEP_DEFAULT_HOUR, SLEEP_DEFAULT_MINUTE, SLEEP_DEFAULT_DELAY_MIN,
        SLEEP_DEFAULT_GAIN_DB, SLEEP_TIME_FIELD_HOUR
    };
    InputEncoder_t encoder = {0};
    (void)stacd;
    (void)exinf;

    while (!sleep_app.ready && !sleep_app.failed)
        tk_dly_tsk(SLEEP_INPUT_POLL_MS);
    if (sleep_app.failed) sleep_app_fault("startup", E_IO);
    __HAL_TIM_SET_COUNTER(&htim8, 0U);
    if (HAL_TIM_Encoder_Start(&htim8, TIM_CHANNEL_ALL) != HAL_OK)
        sleep_app_fault("encoder start", E_IO);
    /* lcd_taskが表示したSLEEP SUPPORTを起点に3秒計る。 */
    tk_dly_tsk(SLEEP_SPLASH_MS);
    input_show_settings(LCD_SCREEN_SETTING_TIME, &settings);

    for (;;) {
        for (unsigned i = 0; i < INPUT_SWITCH_COUNT; ++i)
            input_read_switch(&switches[i]);
        input_adjust_setting(state, &settings, input_read_encoder(&encoder));

        if (switches[INPUT_ENTER].pressed_edge) {
            switch (state) {
            case INPUT_SETTING_TIME: {
                ER err = storage_set_clock(settings.hour, settings.minute);
                if (err < E_OK) sleep_app_fault("clock setting", err);
                state = INPUT_SETTING_DELAY;
                input_show_settings(LCD_SCREEN_SETTING_DELAY, &settings);
                break;
            }
            case INPUT_SETTING_DELAY:
                state = INPUT_SETTING_GAIN;
                input_show_settings(LCD_SCREEN_SETTING_GAIN, &settings);
                break;
            case INPUT_SETTING_GAIN:
                state = INPUT_WAIT_ENTER;
                input_show(LCD_SCREEN_WAIT_ENTER);
                break;
            case INPUT_WAIT_ENTER:
                input_start_monitoring(&settings);
                state = INPUT_MONITORING;
                break;
            case INPUT_MONITORING: {
                ER err = input_finish_monitoring();
                if (err == E_OK) {
                    state = INPUT_WAIT_ENTER;
                    input_show(LCD_SCREEN_WAIT_ENTER);
                } else {
                    /* カード未挿入・I/O失敗でもENTERで再試行できる。 */
                    state = INPUT_SAVE_FAILED;
                    input_show(LCD_SCREEN_SAVE_FAILED);
                }
                break;
            }
            case INPUT_SAVE_FAILED: {
                ER err = input_save_night();
                if (err == E_OK) {
                    state = INPUT_WAIT_ENTER;
                    input_show(LCD_SCREEN_WAIT_ENTER);
                } else {
                    input_show(LCD_SCREEN_SAVE_FAILED);
                }
                break;
            }
            }
        }
        if (state == INPUT_SETTING_TIME &&
            switches[INPUT_MINUTE].pressed_edge &&
            settings.time_field != SLEEP_TIME_FIELD_MINUTE) {
            settings.time_field = SLEEP_TIME_FIELD_MINUTE;
            input_show_settings(LCD_SCREEN_SETTING_TIME, &settings);
        }
        tk_dly_tsk(SLEEP_INPUT_POLL_MS);
    }
}
#endif
