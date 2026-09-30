#include <tk/tkernel.h>

#define UI_EVT_INIT          (1U << 0)
#define UI_EVT_AUDIO_STATE   (1U << 1)
#define UI_EVT_AI_RESULT     (1U << 2)
#define UI_EVT_ERROR         (1U << 3)

typedef enum {
    AUDIO_STATE_STOPPED,
    AUDIO_STATE_STARTING,
    AUDIO_STATE_RECORDING,
    AUDIO_STATE_STOPPING
} AudioState_t;

typedef enum {
    LCD_POWER_ON = 1,
    LCD_SETTING_TIME,
    LCD_SETTING_DELAY,
    LCD_SETTING_MIC,
    LCD_WAIT_KEY,
    LCD_GOOD_NIGHT,
    LCD_WAIT_DELAY,
    LCD_MONITORING_EVENT_CNT,
    LCD_MONITORING_DETECT,
    LCD_GOOD_MORNING,
    LCD_SAVING,
    LCD_COMPLETED,
    LCD_ERROR
} LCDState_t;


typedef struct {
    AudioState_t audio_state;
    LCDState_t  lcd_state;
    AiClassId  class_id;
    float      confidence;
    uint32_t   error_flags;
} UiSnapshot;

static ID ui_event_flag;
static UiSnapshot ui_snapshot;

static void ui_task(INT stacd, void *exinf)
{
    UINT event_pattern;

    lcd_init();
    lcd_show_stopped();

    while (1) {
        ER result = tk_wai_flg(
            ui_event_flag,
            UI_EVT_AUDIO_STATE |
            UI_EVT_AI_RESULT   |
            UI_EVT_ERROR,
            TWF_ORW | TWF_BITCLR,
            &event_pattern,
            TMO_FEVR
        );

        if (result < E_OK) {
            continue;
        }

        /*
         * 実際には短い排他区間でui_snapshotをコピーし、
         * LCDアクセス中は共有データをロックし続けない。
         */
        UiSnapshot snapshot = ui_snapshot;

        if ((event_pattern & UI_EVT_AUDIO_STATE) != 0U) {
            lcd_show_audio_state(snapshot.audio_state);
        }

        if ((event_pattern & UI_EVT_AI_RESULT) != 0U) {
            lcd_show_inference_result(
                snapshot.class_id,
                snapshot.confidence
            );
        }

        if ((event_pattern & UI_EVT_ERROR) != 0U) {
            lcd_show_error(snapshot.error_flags);
        }
    }
}



//* 通知側

void ui_notify_audio_state(AudioState_t state)
{
    /* 共有データ更新は短い排他区間で行う */
    ui_snapshot.audio_state = state;
    tk_set_flg(ui_event_flag, UI_EVT_AUDIO_STATE);
}

void ui_notify_inference_result(const AiResult *result)
{
    /* 共有データ更新は短い排他区間で行う */
    ui_snapshot.class_id  = result->class_id;
    ui_snapshot.confidence = result->confidence;
    tk_set_flg(ui_event_flag, UI_EVT_AI_RESULT);
}