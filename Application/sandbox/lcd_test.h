#ifndef LCD_TEST_H
#define LCD_TEST_H

/* 7bitアドレス。0なら0x27 / 0x3Fを確認。変更後は再ビルドする。 */
#define LCD_TEST_ADDRESS       0U
#define LCD_TEST_PAGE_MS        3000U
/* 0: 全13画面を巡回、1～13: 指定画面を固定表示 */
#define LCD_TEST_FIXED_PAGE     0U

#if LCD_TEST_ADDRESS > 0x77U
#error "LCD_TEST_ADDRESS must be a 7-bit I2C address"
#endif
#if LCD_TEST_FIXED_PAGE > 13U
#error "LCD_TEST_FIXED_PAGE must be 0..13"
#endif
#if LCD_TEST_PAGE_MS < 1U
#error "LCD_TEST_PAGE_MS must be positive"
#endif
#endif
