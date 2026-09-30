#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX

#include "inference.h"
#include "../sleep_app_internal.h"
#include "../lcd/lcd.h"
#include "../storage/storage.h"
#include "ai_pipeline.h"
#include <string.h>

typedef struct {
    int16_t pcm[AI_INPUT_SAMPLES];
    uint32_t sample_count;
    uint64_t trigger_sample;
    volatile AiBufferState state;
} AiInput;

static AiInput ai_buffers[INFERENCE_BUFFER_COUNT];
static volatile UINT active;
static unsigned next_buffer;
static uint32_t event_count;

int inference_busy(void)
{
    if (active) return 1;

    __DMB();

    for (unsigned i = 0; i < INFERENCE_BUFFER_COUNT; ++i)
        if (ai_buffers[i].state != AI_FREE) return 1;

    return 0;
}

ER inference_reset_session(void)
{
    if (inference_busy()) return E_OBJ;

    event_count = 0;
    next_buffer = 0;

    for (unsigned i = 0; i < INFERENCE_BUFFER_COUNT; ++i)
        ai_buffers[i].state = AI_FREE;

    return E_OK;
}

ER inference_submit(const int16_t *pcm, uint32_t count, uint64_t position)
{
    /*
     * AIは16 kHz・1秒 = 16000 samples固定。
     */
    if (count != AI_INPUT_SAMPLES || pcm == NULL) return E_OBJ;

    if (!sleep_app.ready || sleep_app.failed || !sleep_app.monitoring)
        return E_OBJ;

    /*
     * 生産者はdTrig_taskのみ。
     * 推論中でも空いている面へ次の要求を積む。
     */
    unsigned index = next_buffer;

    for (unsigned checked = 0;
         checked < INFERENCE_BUFFER_COUNT;
         ++checked) {

        if (ai_buffers[index].state == AI_FREE)
            break;

        index =
            (index + 1U) %
            INFERENCE_BUFFER_COUNT;
    }

    AiInput *input = &ai_buffers[index];

    if (input->state != AI_FREE)
        return E_OBJ;

    input->state = AI_WRITING;

    memcpy(input->pcm,
           pcm,
           count * sizeof(*pcm));

    input->sample_count = count;
    input->trigger_sample = position;

    __DMB();

    input->state = AI_READY;

    /*
     * フラグのbit番号がAIバッファ番号を表す。
     * 各面のREADY通知は保持される。
     */
    ER err =
        tk_set_flg(
            sleep_app.inference_flag,
            1U << index);

    if (err < E_OK) {
        input->state = AI_FREE;
        return err;
    }

    next_buffer =
        (index + 1U) %
        INFERENCE_BUFFER_COUNT;

    return E_OK;
}

void inference_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    if (!ai_pipeline_init())
        sleep_app_fault("AI init", E_IO);

    for (;;) {
        UINT buffers;

        ER err =
            tk_wai_flg(
                sleep_app.inference_flag,
                3U,
                TWF_ORW | TWF_BITCLR,
                &buffers,
                TMO_FEVR);

        if (err < E_OK)
            sleep_app_fault("inference wait", err);

        for (unsigned i = 0;
             i < INFERENCE_BUFFER_COUNT;
             ++i) {

            if ((buffers & (1U << i)) == 0U)
                continue;

            AiInput *input =
                &ai_buffers[i];

            if (input->state != AI_READY)
                sleep_app_fault(
                    "AI buffer state",
                    E_OBJ);

            active = 1;
            input->state = AI_INFERENCE;

            err =
                lcd_show_and_wait(
                    LCD_SCREEN_TRIGGER,
                    event_count);

            if (err < E_OK)
                sleep_app_fault(
                    "LCD trigger",
                    err);

            ai_pipeline_result_t ai_result;

            if (!ai_pipeline_predict(
                    input->pcm,
                    &ai_result)) {

                input->state = AI_FREE;
                active = 0;
                continue;
            }

            SleepEvent result = {
                .sample_position =
                    input->trigger_sample,

                .event_class =
                    (EventClass)ai_result.class_id,

                .confidence_q8 =
                    ai_result.confidence_q8
            };

            input->state = AI_FREE;

            if (result.event_class != EVENT_OTHER) {
                err =
                    storage_notify_result(
                        &result);

                if (err < E_OK &&
                    err != E_QOVR)
                    sleep_app_fault(
                        "record notify",
                        err);

                if (err == E_OK &&
                    event_count < 99999999U)
                    ++event_count;
            }

            if (sleep_app.monitoring &&
                (result.event_class == EVENT_SNORE ||
                 result.event_class == EVENT_COUGH)) {

                err =
                    lcd_show_and_wait(
                        result.event_class == EVENT_SNORE
                            ? LCD_SCREEN_SNORE : LCD_SCREEN_COUGH,
                        event_count);

                if (err < E_OK)
                    sleep_app_fault(
                        "LCD result",
                        err);

                tk_dly_tsk(
                    SLEEP_RESULT_DISPLAY_MS);
            }

            if (sleep_app.monitoring) {
                err =
                    lcd_show_and_wait(
                        LCD_SCREEN_MONITOR,
                        event_count);

                if (err < E_OK)
                    sleep_app_fault(
                        "LCD monitor",
                        err);
            }

            /*
             * この要求の結果表示まで完了。
             * 待機中の別バッファは次に処理する。
             */
            active = 0;
        }
    }
}

#endif
