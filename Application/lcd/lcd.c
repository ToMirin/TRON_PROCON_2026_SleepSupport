#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX
#include "lcd.h"
#include "lcd_driver.h"
#include "../sleep_app_internal.h"
#include <tm/tmonitor.h>

#define LCD_UPDATE_REQUEST 1U

static LcdScreen requested_screen;
static uint32_t requested_value;
static SleepSettings requested_settings = {
    SLEEP_DEFAULT_HOUR, SLEEP_DEFAULT_MINUTE, SLEEP_DEFAULT_DELAY_MIN,
    SLEEP_DEFAULT_GAIN_DB, SLEEP_TIME_FIELD_HOUR
};

static ER lcd_submit_and_wait(LcdScreen screen, uint32_t value,
                              const SleepSettings *settings)
{
    UINT ack;
    if (sleep_app.failed) return E_IO;
    ER err = tk_loc_mtx(sleep_app.lcd_mutex, TMO_FEVR);
    if (err < E_OK) return err;
    requested_screen = screen;
    requested_value = value;
    if (settings != NULL) requested_settings = *settings;
    __DMB();
    err = tk_set_flg(sleep_app.lcd_flag, LCD_UPDATE_REQUEST);
    if (err >= E_OK)
        err = tk_wai_flg(sleep_app.lcd_ack_flag, 1U,
                         TWF_ORW | TWF_BITCLR, &ack, TMO_FEVR);
    ER unlock = tk_unl_mtx(sleep_app.lcd_mutex);
    return err < E_OK ? err : unlock;
}

ER lcd_show_and_wait(LcdScreen screen, uint32_t value)
{
    return lcd_submit_and_wait(screen, value, NULL);
}

ER lcd_show_settings_and_wait(LcdScreen screen, const SleepSettings *settings)
{
    if (settings == NULL) return E_PAR;
    return lcd_submit_and_wait(screen, 0, settings);
}

static void lcd_render(LcdScreen screen, uint32_t value)
{
    ResetLCD(0, 31);
    switch (screen) {
    case LCD_SCREEN_SPLASH:
        SetLCDItem(LCD_SLEEP_SUPPORT, 0, 15);
        SetLCDItem(LCD_STARS, 16, 31);
        break;
    case LCD_SCREEN_SETTING_TIME:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_TIME_SETTING, 8, 15);
        SetLCDItem(LCD_NUMBER_ZERO(requested_settings.hour), 27, 28);
        SetLCDItem(LCD_COLON, 29, 29);
        SetLCDItem(LCD_NUMBER_ZERO(requested_settings.minute), 30, 31);
        SetLCDItem(LCD_UNDERLINE,
                   requested_settings.time_field == SLEEP_TIME_FIELD_HOUR ? 27 : 30,
                   requested_settings.time_field == SLEEP_TIME_FIELD_HOUR ? 27 : 30);
        break;
    case LCD_SCREEN_SETTING_DELAY:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_DELAY_SETTING, 8, 15);
        SetLCDItem(LCD_NUMBER(requested_settings.delay_minutes), 27, 28);
        SetLCDItem(LCD_MIN, 29, 31);
        SetLCDItem(LCD_UNDERLINE,
                   requested_settings.delay_minutes < 10U ? 28 : 27,
                   requested_settings.delay_minutes < 10U ? 28 : 27);
        break;
    case LCD_SCREEN_SETTING_GAIN:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_GAIN_SETTING, 8, 15);
        SetLCDItem(LCD_PLUS, 27, 27);
        SetLCDItem(LCD_NUMBER(requested_settings.gain_db), 28, 29);
        SetLCDItem(LCD_DB, 30, 31);
        SetLCDItem(LCD_UNDERLINE,
                   requested_settings.gain_db < 10U ? 29 : 28,
                   requested_settings.gain_db < 10U ? 29 : 28);
        break;
    case LCD_SCREEN_WAIT_ENTER:
        SetLCDItem(LCD_PLEASE_PRESS, 0, 15);
        SetLCDItem(LCD_ENTER_KEY, 16, 31);
        break;
    case LCD_SCREEN_GOOD_NIGHT:
        SetLCDItem(LCD_START, 0, 15);
        SetLCDItem(LCD_GOOD_NIGHT, 16, 31);
        break;
    case LCD_SCREEN_DELAY:
        SetLCDItem(LCD_DELAY, 0, 15);
        SetLCDItem(LCD_NUMBER(requested_settings.delay_minutes), 21, 22);
        SetLCDItem(LCD_LEFT, 23, 31);
        break;
    case LCD_SCREEN_MONITOR:
        SetLCDItem(LCD_TEXT("Monitoring"), 0, 15);
        SetLCDItem(LCD_EVENT, 16, 23);
        SetLCDItem(LCD_NUMBER((int32_t)value), 24, 31);
        break;
    case LCD_SCREEN_TRIGGER:
    case LCD_SCREEN_SNORE:
    case LCD_SCREEN_COUGH:
        SetLCDItem(LCD_TEXT("Monitoring"), 0, 15);
        SetLCDItem(LCD_DETECT, 16, 23);
        SetLCDItem(screen == LCD_SCREEN_TRIGGER ? LCD_TRIG :
                   screen == LCD_SCREEN_SNORE ? LCD_SNORE : LCD_COUGH, 24, 31);
        break;
    case LCD_SCREEN_GOOD_MORNING:
        SetLCDItem(LCD_STOP, 0, 15);
        SetLCDItem(LCD_GOOD_MORNING, 16, 31);
        break;
    case LCD_SCREEN_SAVING:
        SetLCDItem(LCD_SAVING, 0, 15);
        SetLCDItem(LCD_SD, 16, 31);
        break;
    case LCD_SCREEN_SAVE_FAILED:
        SetLCDItem(LCD_TEXT("Save failed"), 0, 15);
        SetLCDItem(LCD_TEXT("Press ENTER"), 16, 31);
        break;
    case LCD_SCREEN_SAVE_COMPLETE:
        SetLCDItem(LCD_TEXT("All completed."), 0, 15);
        break;
    }
}

/* 同じ設定画面内の値変更では、数値とカーソルがある末尾5文字だけ送る。
 * 32文字の全転送は画面遷移時に限定し、エンコーダ操作への追従を速める。 */
static void lcd_update_range(LcdScreen screen, LcdScreen displayed_screen,
                             unsigned *first, unsigned *last)
{
    *first = 0;
    *last = 31;
    if (screen == displayed_screen &&
        (screen == LCD_SCREEN_SETTING_TIME ||
         screen == LCD_SCREEN_SETTING_DELAY ||
         screen == LCD_SCREEN_SETTING_GAIN)) {
        *first = 27;
    }
}

void lcd_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

#if SLEEP_LCD_HEADLESS

    /*
     * LCDなし検証用。
     * 実LCDへのI2Cアクセスだけ省略し、
     * ready / request / ack の同期経路は通常時と同じにする。
     */
    tm_putstring((UB *)"LCD: headless mode\n");

    if (!sleep_app.failed)
        sleep_app.ready = 1;

    for (;;) {
        UINT event;

        ER err = tk_wai_flg(sleep_app.lcd_flag,
                           LCD_UPDATE_REQUEST,
                           TWF_ORW | TWF_BITCLR,
                           &event,
                           TMO_FEVR);

        if (err < E_OK)
            sleep_app_fault("LCD wait", err);

        __DMB();

        switch (requested_screen) {
        case LCD_SCREEN_SPLASH:
            tm_putstring((UB *)"LCD: SPLASH\n");
            break;

        case LCD_SCREEN_SETTING_TIME:
            tm_printf((UB *)"LCD: SETTING_TIME %02u:%02u field=%s\n",
                      (unsigned)requested_settings.hour,
                      (unsigned)requested_settings.minute,
                      requested_settings.time_field == SLEEP_TIME_FIELD_HOUR
                          ? "hour"
                          : "minute");
            break;

        case LCD_SCREEN_SETTING_DELAY:
            tm_printf((UB *)"LCD: SETTING_DELAY %u min\n",
                      (unsigned)requested_settings.delay_minutes);
            break;

        case LCD_SCREEN_SETTING_GAIN:
            tm_printf((UB *)"LCD: SETTING_GAIN +%u dB\n",
                      (unsigned)requested_settings.gain_db);
            break;

        case LCD_SCREEN_WAIT_ENTER:
            tm_putstring((UB *)"LCD: WAIT_ENTER\n");
            break;

        case LCD_SCREEN_GOOD_NIGHT:
            tm_putstring((UB *)"LCD: GOOD_NIGHT\n");
            break;

        case LCD_SCREEN_DELAY:
            tm_printf((UB *)"LCD: DELAY %lu\n",
                      (unsigned long)requested_value);
            break;

        case LCD_SCREEN_MONITOR:
            tm_printf((UB *)"LCD: MONITOR event=%lu\n",
                      (unsigned long)requested_value);
            break;

        case LCD_SCREEN_TRIGGER:
            tm_putstring((UB *)"LCD: TRIGGER\n");
            break;

        case LCD_SCREEN_SNORE:
            tm_putstring((UB *)"LCD: SNORE\n");
            break;

        case LCD_SCREEN_COUGH:
            tm_putstring((UB *)"LCD: COUGH\n");
            break;

        case LCD_SCREEN_GOOD_MORNING:
            tm_putstring((UB *)"LCD: GOOD_MORNING\n");
            break;

        case LCD_SCREEN_SAVING:
            tm_putstring((UB *)"LCD: SAVING\n");
            break;

        case LCD_SCREEN_SAVE_FAILED:
            tm_putstring((UB *)"LCD: SAVE_FAILED\n");
            break;

        case LCD_SCREEN_SAVE_COMPLETE:
            tm_putstring((UB *)"LCD: SAVE_COMPLETE\n");
            break;
        }

        err = tk_set_flg(sleep_app.lcd_ack_flag, 1U);

        if (err < E_OK)
            sleep_app_fault("LCD ack", err);
    }

#else

    if (lcd_init() != HAL_OK)
        sleep_app_fault("LCD init", E_IO);

    lcd_render(LCD_SCREEN_SPLASH, 0);

    if (UpdateLCD(0, 31) != HAL_OK)
        sleep_app_fault("LCD write", E_IO);

    LcdScreen displayed_screen = LCD_SCREEN_SPLASH;

    if (!sleep_app.failed)
        sleep_app.ready = 1;

    for (;;) {
        UINT event;

        ER err = tk_wai_flg(sleep_app.lcd_flag,
                           LCD_UPDATE_REQUEST,
                           TWF_ORW | TWF_BITCLR,
                           &event,
                           TMO_FEVR);

        if (err < E_OK)
            sleep_app_fault("LCD wait", err);

        __DMB();

        unsigned first, last;

        lcd_update_range(requested_screen,
                         displayed_screen,
                         &first,
                         &last);

        lcd_render(requested_screen,
                   requested_value);

        if (UpdateLCD(first, last) != HAL_OK)
            sleep_app_fault("LCD write", E_IO);

        displayed_screen = requested_screen;

        err = tk_set_flg(sleep_app.lcd_ack_flag, 1U);

        if (err < E_OK)
            sleep_app_fault("LCD ack", err);
    }

#endif
}
#endif
