#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "sleep_app_config.h"

#if (defined(SW_TEST) + \
     defined(ENC_TEST) + \
     defined(DMA_TEST) + \
     defined(LCD_TEST) + \
     defined(TIME_TEST)) > 1
#error "Select only one of XX_TEST"
#endif

#if (defined(SW_TEST) || \
     defined(ENC_TEST) || \
     defined(DMA_TEST) || \
     defined(LCD_TEST) || \
     defined(TIME_TEST)) && \
    !defined(SANDBOX)
#error "Sandbox tests require SANDBOX"
#endif


#if defined(SANDBOX) && defined(SW_TEST)
// ==================================================
//* SW_TEST: GPIOの入力を読み取り、対応するLEDを点灯させる
// ==================================================
#include "sandbox/sw_test.c"

EXPORT INT usermain(void)
{
    ER err = sw_test_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "sw_test_start failed: %d\n",
            (int)err);
    }
    else {
        tm_putstring(
            (UB *)
            "Switch/LED task started.\n");
    }

    /*
     * 初期タスクを待ち状態にし、
     * テストタスクを実行可能にする。
     */
    while (1) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}
#endif
// ==================================================


#if defined(SANDBOX) && defined(ENC_TEST)
// ==================================================
//* ENC_TEST: エンコーダのA/B信号を読み取り、回転方向を判定する
// ==================================================
#include "sandbox/enc_test.c"

EXPORT INT usermain(void)
{
    ER err = enc_test_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "enc_test_start failed: %d\n",
            (int)err);
    }
    else {
        tm_putstring(
            (UB *)
            "Encoder task started.\n");
    }

    /*
     * 初期タスクを待機させ、
     * ENCタスクを実行する。
     */
    while (1) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}
#endif
// ==================================================


#if defined(SANDBOX) && defined(DMA_TEST)
// ==================================================
//* DMA_TEST: I2S/DMA・マイク入力確認
// ==================================================
#include "sandbox/dma_test.c"
#include "sandbox/audio_test.c"

EXPORT INT usermain(void)
{
    ER err = audio_test_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "audio_test_start failed: %d\n",
            (int)err);
    }
    else {
        tm_putstring(
            (UB *)
            "Audio task started.\n");
    }

    /*
     * 初期タスクを待機させ、
     * DMAタスクとaudio_taskを実行する。
     */
    while (1) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}
#endif
// ==================================================


#if defined(SANDBOX) && defined(LCD_TEST)
// ==================================================
//* LCD_TEST
// ==================================================
#include "sandbox/lcd_test.c"

EXPORT INT usermain(void)
{
    ER err = lcd_test_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "lcd_test_start failed: %d\n",
            (int)err);
    }
    else {
        tm_putstring(
            (UB *)
            "LCD display test started.\n");
    }

    while (1) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}
#endif
// ==================================================


#if defined(SANDBOX) && defined(TIME_TEST)
// ==================================================
//* TIME_TEST: D-09 sample_position時刻変換確認
// ==================================================
#include "sandbox/time_test.c"

EXPORT INT usermain(void)
{
    ER err = time_test_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "time_test_start failed: %d\n",
            (int)err);
    }
    else {
        tm_putstring(
            (UB *)
            "TIME_TEST completed successfully.\n");
    }

    while (1) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}
#endif
// ==================================================


/*
 * 通常のアプリケーション
 */
#if !defined(SANDBOX)

#include "sleep_app.h"

EXPORT INT usermain(void)
{
    ER err = sleep_app_start();

    if (err < E_OK) {
        tm_printf(
            (UB *)
            "sleep_app_start failed: %d\n",
            (int)err);
    }

    for (;;) {
        tk_slp_tsk(TMO_FEVR);
    }

    return 0;
}

#endif /* !SANDBOX */