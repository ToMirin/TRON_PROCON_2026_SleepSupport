#ifndef SLEEP_APP_CONFIG_H
#define SLEEP_APP_CONFIG_H

/* 0: 通常システム、1: 個別sandbox。全翻訳単位で同じ選択を使う。 */
#ifndef SLEEP_APP_SANDBOX
#define SLEEP_APP_SANDBOX 0
#endif
#if SLEEP_APP_SANDBOX
#define SANDBOX
/*
 * EVT-04A:
 * sample_position時刻変換テスト。
 *
 * テスト完了後は削除またはコメントアウトし、
 * SLEEP_APP_SANDBOXも0へ戻す。
 */
#define TIME_TEST
#if !defined(SW_TEST) && !defined(ENC_TEST) && !defined(DMA_TEST) && !defined(LCD_TEST) && !defined(TIME_TEST)
#define LCD_TEST
#endif
#endif

/*
 * 0: 実LCDを使用
 * 1: LCDなしで通常アプリを動作させる
 *
 * 通常は実LCDを使用。LCDなし検証時のみ1を指定する。
 */
#ifndef SLEEP_LCD_HEADLESS
#define SLEEP_LCD_HEADLESS 0
#endif

#define SLEEP_AUDIO_PRIORITY      5
#define SLEEP_DETECTION_PRIORITY  8
#define SLEEP_INPUT_PRIORITY      10 /* 暫定: SW ISRの代わりにポーリング */
#define SLEEP_INFERENCE_PRIORITY  12
#define SLEEP_RECORD_PRIORITY     16
#define SLEEP_LCD_PRIORITY        18
#define SLEEP_INPUT_POLL_MS       10
#define SLEEP_SPLASH_MS           3000
#define SLEEP_GOOD_NIGHT_MS        3000
#define SLEEP_GOOD_MORNING_MS     3000
#define SLEEP_SAVE_COMPLETE_MS    3000
#define SLEEP_INFERENCE_DEMO_MS   1000
#define SLEEP_RESULT_DISPLAY_MS   2000
#define SLEEP_DEFAULT_HOUR        23
#define SLEEP_DEFAULT_MINUTE      59
#define SLEEP_DEFAULT_DELAY_MIN   0
#define SLEEP_DEFAULT_GAIN_DB     18
#define SLEEP_DELAY_MIN_MIN       0
#define SLEEP_DELAY_MIN_MAX       99
#define SLEEP_GAIN_MIN_DB         0
#define SLEEP_GAIN_MAX_DB         48
#define SLEEP_GAIN_STEP_DB        6
#define SLEEP_ENCODER_COUNTS_STEP 4
#endif
