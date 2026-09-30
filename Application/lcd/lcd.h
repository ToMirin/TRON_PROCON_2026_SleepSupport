#ifndef SLEEP_LCD_H
#define SLEEP_LCD_H
#include <tk/tkernel.h>
#include <stdint.h>
#include "../sleep_settings.h"
typedef enum {
    LCD_SCREEN_SPLASH,
    LCD_SCREEN_SETTING_TIME,
    LCD_SCREEN_SETTING_DELAY,
    LCD_SCREEN_SETTING_GAIN,
    LCD_SCREEN_WAIT_ENTER,
    LCD_SCREEN_GOOD_NIGHT,
    LCD_SCREEN_DELAY,
    LCD_SCREEN_MONITOR,
    LCD_SCREEN_TRIGGER,
    LCD_SCREEN_SNORE,
    LCD_SCREEN_COUGH,
    LCD_SCREEN_GOOD_MORNING,
    LCD_SCREEN_SAVING,
    LCD_SCREEN_SAVE_FAILED,
    LCD_SCREEN_SAVE_COMPLETE,
    
    NUM_LCD_SCREENS
} LcdScreen;

void lcd_task(INT stacd, void *exinf);
/* 複数タスクからの表示要求は内部mutexで直列化する。 */
ER lcd_show_and_wait(LcdScreen screen, uint32_t value);
/* 設定画面と待機画面では、入力タスクが保持する値のコピーを表示する。 */
ER lcd_show_settings_and_wait(LcdScreen screen, const SleepSettings *settings);
#endif
