#ifndef LCD_PAGES_H
#define LCD_PAGES_H

/* 固定値はLCDItemとして保有。将来は値変更後にSetLCDItemで再配置できる。 */
static LCDItem lcd_hour_setting = {.kind = LCD_KIND_NUMBER, .number = 24, .zero_pad = 1};
static LCDItem lcd_minute = {.kind = LCD_KIND_NUMBER, .number = 59, .zero_pad = 1};
static LCDItem lcd_delay_minutes = {.kind = LCD_KIND_NUMBER, .number = 5};
static LCDItem lcd_gain = {.kind = LCD_KIND_NUMBER, .number = 6};
static LCDItem lcd_events = {.kind = LCD_KIND_NUMBER, .number = 1024};

static void lcd_test_time(void)
{
    SetLCDItem(LCD_TIME, 0, 7);
    SetLCDItem(LCD_NUMBER_ZERO(23), 8, 9);
    SetLCDItem(LCD_COLON, 10, 10);
    SetLCDItem(lcd_minute, 11, 12);
    SetLCDItem(LCD_COLON, 13, 13);
    SetLCDItem(LCD_NUMBER_ZERO(30), 14, 15);
}

/* 全画面切替では0～31をリセット。部分変更は対象範囲だけで同じ手順を使う。 */
static void lcd_test_page(unsigned page)
{
    ResetLCD(0, 31);
    switch (page) {
    case 1:
        SetLCDItem(LCD_SLEEP_SUPPORT, 0, 15);
        SetLCDItem(LCD_STARS, 16, 31);
        break;
    case 2:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_TIME_SETTING, 8, 15);
        SetLCDItem(lcd_hour_setting, 27, 28);
        SetLCDItem(LCD_COLON, 29, 29);
        SetLCDItem(lcd_minute, 30, 31);
        SetLCDItem(LCD_UNDERLINE, 27, 27);
        break;
    case 3:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_DELAY_SETTING, 8, 15);
        SetLCDItem(lcd_delay_minutes, 28, 28);
        SetLCDItem(LCD_MIN, 29, 31);
        SetLCDItem(LCD_UNDERLINE, 28, 28);
        break;
    case 4:
        SetLCDItem(LCD_SETTING, 0, 7);
        SetLCDItem(LCD_GAIN_SETTING, 8, 15);
        SetLCDItem(LCD_BLOCK, 16, 23);
        SetLCDItem(LCD_PLUS, 28, 28);
        SetLCDItem(lcd_gain, 29, 29);
        SetLCDItem(LCD_DB, 30, 31);
        SetLCDItem(LCD_UNDERLINE, 29, 29);
        break;
    case 5:
        SetLCDItem(LCD_PLEASE_PRESS, 0, 15);
        SetLCDItem(LCD_ENTER_KEY, 16, 31);
        break;
    case 6:
        SetLCDItem(LCD_START, 0, 15);
        SetLCDItem(LCD_GOOD_NIGHT, 16, 31);
        break;
    case 7:
        SetLCDItem(LCD_DELAY, 0, 15);
        SetLCDItem(lcd_delay_minutes, 22, 22);
        SetLCDItem(LCD_LEFT, 23, 31);
        break;
    case 8:
        lcd_test_time();
        SetLCDItem(LCD_EVENT, 16, 23);
        SetLCDItem(lcd_events, 24, 27);
        break;
    case 9:
    case 10:
        lcd_test_time();
        SetLCDItem(LCD_DETECT, 16, 23);
        SetLCDItem(page == 9 ? LCD_TRIG : LCD_COUGH, 24, 31);
        break;
    case 11:
        SetLCDItem(LCD_STOP, 0, 15);
        SetLCDItem(LCD_GOOD_MORNING, 16, 31);
        break;
    case 12:
        SetLCDItem(LCD_SAVING, 0, 15);
        SetLCDItem(LCD_SD, 16, 31);
        break;
    case 13:
        SetLCDItem(LCD_COMPLETED, 0, 15);
        break;
    }
}
#endif
