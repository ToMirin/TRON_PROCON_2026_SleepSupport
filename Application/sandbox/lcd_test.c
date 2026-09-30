/* app_main.cから取り込む表示専用テスト。独立してビルドしない。 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "main.h"
#include "lcd_test.h"
#include "lcd_items.h"

extern I2C_HandleTypeDef hi2c1;

/* Freenove: P0=RS, P1=RW, P2=E, P3=backlight, P4..7=D4..7 */
#define LCD_RS  0x01U
#define LCD_EN  0x04U
#define LCD_BL  0x08U
#define LCD_COLS 16U
#define LCD_ROWS 2U

static uint16_t lcd_address;
static HAL_StatusTypeDef lcd_status;
static uint8_t lcd_frame[32];
static int lcd_underline = -1;
static int lcd_visible_underline = -1;

/* エラー後は送信を止める。途中からの継続は4bit転送の位相が不確か。 */
static void lcd_port(uint8_t bits)
{
    if (lcd_status == HAL_OK) {
        lcd_status = HAL_I2C_Master_Transmit(&hi2c1, lcd_address,
                                             &bits, 1, 100);
    }
}

static void lcd_nibble(uint8_t bits)
{
    bits |= LCD_BL; /* RWは常に0（書き込み） */
    lcd_port(bits);
    lcd_port(bits | LCD_EN);
    lcd_port(bits);
    /* I2C転送自体でEパルス幅を確保。命令完了待ちはタスクを解放。 */
    tk_dly_tsk(1);
}

static void lcd_byte(uint8_t value, uint8_t rs)
{
    lcd_nibble((value & 0xf0U) | rs);
    lcd_nibble((uint8_t)(value << 4) | rs);
}

static void lcd_command(uint8_t value)
{
    lcd_byte(value, 0);
    if (value == 0x01U || value == 0x02U) tk_dly_tsk(3);
}

static HAL_StatusTypeDef lcd_init(void)
{
    static const uint8_t candidates[] = {0x27, 0x3f};
    lcd_underline = -1;
    lcd_visible_underline = -1;
    for (unsigned i = 0; i < 32; ++i) lcd_frame[i] = ' ';
    lcd_status = HAL_ERROR;
    tk_dly_tsk(50);
    if (LCD_TEST_ADDRESS != 0U) {
        lcd_address = (uint16_t)(LCD_TEST_ADDRESS << 1);
        lcd_status = HAL_I2C_IsDeviceReady(&hi2c1, lcd_address, 2, 100);
    } else {
        for (unsigned i = 0; i < sizeof(candidates); ++i) {
            lcd_address = (uint16_t)(candidates[i] << 1);
            lcd_status = HAL_I2C_IsDeviceReady(&hi2c1, lcd_address, 2, 100);
            if (lcd_status == HAL_OK) break;
        }
    }
    if (lcd_status != HAL_OK) return lcd_status;
    tm_printf((UB *)"LCD I2C address: 0x%02x\n", lcd_address >> 1);
    lcd_port(LCD_BL);
    /* 電源投入時・MCUだけのリセット時のどちらからも4bitモードへ戻す。 */
    lcd_nibble(0x30); tk_dly_tsk(5);
    lcd_nibble(0x30); tk_dly_tsk(5);
    lcd_nibble(0x30);
    lcd_nibble(0x20);
    lcd_command(0x28); /* 4bit, 2行, 5x8 */
    lcd_command(0x08); /* 表示OFF */
    lcd_command(0x01);
    lcd_command(0x06); /* 右へ進む、表示シフトなし */
    /* CGRAMの文字0: ROMの文字表によらない塗りつぶしブロック。 */
    lcd_command(0x40);
    for (unsigned i = 0; i < 8; ++i) lcd_byte(0x1f, LCD_RS);
    lcd_command(0x80);
    lcd_command(0x0c); /* 表示ON、カーソル・点滅OFF */
    return lcd_status;
}

/* APIはLCDタスクから呼ぶ。範囲は0～31、両端を含む。 */
static int lcd_valid_range(unsigned first, unsigned last)
{
    return first <= last && last < 32;
}

/* 配列の指定範囲を消去。LCD本体への送信はUpdateLCDで行う。 */
static int ResetLCD(unsigned first, unsigned last)
{
    if (!lcd_valid_range(first, last)) return 0;
    for (unsigned i = first; i <= last; ++i) lcd_frame[i] = ' ';
    if (lcd_underline >= (int)first && lcd_underline <= (int)last)
        lcd_underline = -1;
    return 1;
}

/* ASCIIの表示可能文字を扱う。制御文字・非ASCIIバイトは?に置き換える。
 * LCDのROMによってバックスラッシュ等の字形は異なる。UTF-8日本語は対象外。
 */
static uint8_t lcd_ascii(unsigned char ch)
{
    return ch >= 0x20 && ch <= 0x7e ? ch : '?';
}

/* 成功=1、不正な範囲/部品=0。文字列は左寄せ・切り詰め、数値は右寄せ。
 * 内容を即座にコピーするため、Set後に元の変数を変更しても表示配列は変わらない。
 * 下線以外は指定範囲を空白に戻してから配置する。
 */
static int SetLCDItem(LCDItem item, unsigned first, unsigned last)
{
    if (!lcd_valid_range(first, last)) return 0;
    if (item.kind < LCD_KIND_STRING || item.kind > LCD_KIND_BLOCK) return 0;
    if (item.kind == LCD_KIND_STRING && item.text == 0) return 0;
    if (item.kind == LCD_KIND_UNDERLINE) {
        if (first != last) return 0; /* ハードウェアカーソルは1文字のみ */
        lcd_underline = (int)first;
        return 1;
    }
    ResetLCD(first, last);
    unsigned width = last - first + 1;
    switch (item.kind) {
    case LCD_KIND_STRING:
        for (unsigned i = 0; i < width && item.text[i] != '\0'; ++i)
            lcd_frame[first + i] = lcd_ascii((unsigned char)item.text[i]);
        break;
    case LCD_KIND_CHAR:
        lcd_frame[first] = lcd_ascii((unsigned char)item.character);
        break;
    case LCD_KIND_NUMBER: {
        char reversed[10];
        unsigned digits = 0;
        int negative = item.number < 0;
        /* INT32_MINでも符号付き整数のオーバーフローを起こさない。 */
        uint32_t magnitude = negative ? 0U - (uint32_t)item.number : (uint32_t)item.number;
        do {
            reversed[digits++] = (char)('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (digits + (unsigned)negative > width) {
            for (unsigned i = first; i <= last; ++i) lcd_frame[i] = '#';
            break;
        }
        unsigned padding = width - digits - (unsigned)negative;
        unsigned pos = first;
        if (!item.zero_pad) pos += padding;
        if (negative) lcd_frame[pos++] = '-';
        if (item.zero_pad)
            for (unsigned i = 0; i < padding; ++i) lcd_frame[pos++] = '0';
        while (digits != 0) lcd_frame[pos++] = (uint8_t)reversed[--digits];
        break;
    }
    case LCD_KIND_BLOCK:
        for (unsigned i = first; i <= last; ++i) lcd_frame[i] = 0;
        break;
    default:
        break;
    }
    return 1;
}

static uint8_t lcd_ddram(unsigned position)
{
    return (uint8_t)(0x80U | (position < 16 ? position : 0x40U + position - 16));
}



/* 指定範囲だけ送信する。15→16で実機の2行目アドレスに切り替える。
 * 範囲外の表示・下線は保持する。通信失敗後はlcd_initからの再初期化が必要。
 */
static HAL_StatusTypeDef UpdateLCD(unsigned first, unsigned last)
{
    // 引数は0～31の範囲で、first<=lastであること。範囲外はHAL_ERRORを返す。
    if (!lcd_valid_range(first, last)) return HAL_ERROR;
    if (lcd_status != HAL_OK) return lcd_status;

    int cursor = lcd_visible_underline;

    if (cursor >= (int)first && cursor <= (int)last) cursor = -1;
    if (lcd_underline >= (int)first && lcd_underline <= (int)last)
        cursor = lcd_underline;
    lcd_command(0x0c);
    lcd_command(lcd_ddram(first));
    for (unsigned i = first; i <= last; ++i) {
        if (i == 16 && i != first) lcd_command(lcd_ddram(i));
        lcd_byte(lcd_frame[i], LCD_RS);
    }
    if (cursor >= 0) {
        lcd_command(lcd_ddram((unsigned)cursor));
        lcd_command(0x0e);
    }
    if (lcd_status == HAL_OK) lcd_visible_underline = cursor;
    return lcd_status;
}

#include "lcd_pages.h"

static void lcd_test_task(INT stacd, void *exinf)
{
    unsigned page = LCD_TEST_FIXED_PAGE != 0U ? LCD_TEST_FIXED_PAGE : 1U;
    (void)stacd;
    (void)exinf;
    if (lcd_init() == HAL_OK) {
        for (;;) {
            lcd_test_page(page);
            if (UpdateLCD(0, 31) != HAL_OK) break;
            tm_printf((UB *)"LCD page %u/13\n", page);
            if (LCD_TEST_FIXED_PAGE != 0U) {
                for (;;) tk_slp_tsk(TMO_FEVR);
            }
            tk_dly_tsk(LCD_TEST_PAGE_MS);
            page = (page == 13) ? 1 : page + 1;
        }
    }
    tm_printf((UB *)"LCD failed: status=%d, I2C error=0x%lx. Check address/wiring; reset to retry.\n",
              (int)lcd_status, (unsigned long)HAL_I2C_GetError(&hi2c1));
    for (;;) tk_slp_tsk(TMO_FEVR);
}

static ER lcd_test_start(void)
{
    T_CTSK ctsk = {
        .tskatr = TA_HLNG | TA_RNG3,
        .task = lcd_test_task,
        .itskpri = 10,
        .stksz = 1024
    };
    ID id = tk_cre_tsk(&ctsk);
    if (id < E_OK) return (ER)id;
    ER err = tk_sta_tsk(id, 0);
    if (err < E_OK) (void)tk_del_tsk(id);
    return err;
}
