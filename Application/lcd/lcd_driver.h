#ifndef SLEEP_LCD_DRIVER_H
#define SLEEP_LCD_DRIVER_H
#include "main.h"
#include "lcd_items.h"
#define LCD_ADDRESS 0U
HAL_StatusTypeDef lcd_init(void);
int ResetLCD(unsigned first, unsigned last);
int SetLCDItem(LCDItem item, unsigned first, unsigned last);
HAL_StatusTypeDef UpdateLCD(unsigned first, unsigned last);
#endif
