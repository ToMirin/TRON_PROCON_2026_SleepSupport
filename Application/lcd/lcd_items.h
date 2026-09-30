#ifndef LCD_ITEMS_H
#define LCD_ITEMS_H

#include <stdint.h>

/* 値を保持する表示部品。文字列だけは参照なのでSetLCDItemまで有効な文字列を渡す。 */
typedef enum {
    LCD_KIND_STRING, LCD_KIND_CHAR, LCD_KIND_NUMBER,
    LCD_KIND_UNDERLINE, LCD_KIND_BLOCK
} LCDItemKind;

typedef struct {
    LCDItemKind kind;
    const char *text;
    int32_t number;
    char character;
    uint8_t zero_pad;
} LCDItem;

#define LCD_TEXT(value) ((LCDItem){.kind = LCD_KIND_STRING, .text = (value)})
#define LCD_CHAR(value) ((LCDItem){.kind = LCD_KIND_CHAR, .character = (value)})
#define LCD_NUMBER(value) ((LCDItem){.kind = LCD_KIND_NUMBER, .number = (value)})
#define LCD_NUMBER_ZERO(value) ((LCDItem){.kind = LCD_KIND_NUMBER, .number = (value), .zero_pad = 1})

/* 画面で共有する部品。単語を追加する場合はこの一覧に追加する。 */
static const LCDItem LCD_SLEEP_SUPPORT = {.kind = LCD_KIND_STRING, .text = " SLEEP  SUPPORT "};
static const LCDItem LCD_STARS = {.kind = LCD_KIND_STRING, .text = " *****  ******* "};
static const LCDItem LCD_SETTING = {.kind = LCD_KIND_STRING, .text = "Setting "};
static const LCDItem LCD_TIME_SETTING = {.kind = LCD_KIND_STRING, .text = "[TIME]"};
static const LCDItem LCD_DELAY_SETTING = {.kind = LCD_KIND_STRING, .text = "[DELAY]"};
static const LCDItem LCD_GAIN_SETTING = {.kind = LCD_KIND_STRING, .text = "[GAIN]"};
static const LCDItem LCD_MIN = {.kind = LCD_KIND_STRING, .text = "min"};
static const LCDItem LCD_DB = {.kind = LCD_KIND_STRING, .text = "dB"};
static const LCDItem LCD_PLEASE_PRESS = {.kind = LCD_KIND_STRING, .text = "Please Press"};
static const LCDItem LCD_ENTER_KEY = {.kind = LCD_KIND_STRING, .text = "ENTER KEY..."};
/* Moniteringの綴りはUI仕様書に合わせる。 */
static const LCDItem LCD_START = {.kind = LCD_KIND_STRING, .text = "Start Monitering"};
static const LCDItem LCD_STOP = {.kind = LCD_KIND_STRING, .text = "Stop  Monitering"};
static const LCDItem LCD_GOOD_NIGHT = {.kind = LCD_KIND_STRING, .text = "GOOD NIGHT..."};
static const LCDItem LCD_GOOD_MORNING = {.kind = LCD_KIND_STRING, .text = "GOOD MORNING!!"};
static const LCDItem LCD_DELAY = {.kind = LCD_KIND_STRING, .text = "[Delay]"};
static const LCDItem LCD_LEFT = {.kind = LCD_KIND_STRING, .text = " min left"};
static const LCDItem LCD_TIME = {.kind = LCD_KIND_STRING, .text = "[Time]"};
static const LCDItem LCD_EVENT = {.kind = LCD_KIND_STRING, .text = "[Event]"};
static const LCDItem LCD_DETECT = {.kind = LCD_KIND_STRING, .text = "[Detect]"};
static const LCDItem LCD_TRIG = {.kind = LCD_KIND_STRING, .text = "TRIG"};
static const LCDItem LCD_SNORE = {.kind = LCD_KIND_STRING, .text = "SNORE"};
static const LCDItem LCD_COUGH = {.kind = LCD_KIND_STRING, .text = "COUGH"};
static const LCDItem LCD_SAVING = {.kind = LCD_KIND_STRING, .text = "Saving to"};
static const LCDItem LCD_SD = {.kind = LCD_KIND_STRING, .text = "SD card..."};
static const LCDItem LCD_COMPLETED = {.kind = LCD_KIND_STRING, .text = "All completed."};
static const LCDItem LCD_COLON = {.kind = LCD_KIND_CHAR, .character = ':'};
static const LCDItem LCD_PLUS = {.kind = LCD_KIND_CHAR, .character = '+'};
static const LCDItem LCD_UNDERLINE = {.kind = LCD_KIND_UNDERLINE};
static const LCDItem LCD_BLOCK = {.kind = LCD_KIND_BLOCK};
#endif
