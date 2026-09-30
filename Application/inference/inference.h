#ifndef SLEEP_INFERENCE_H
#define SLEEP_INFERENCE_H
#include "../audio/audio.h"
#define INFERENCE_BUFFER_COUNT 2U
/* AI入力は16kHz・1秒のPCM。 */
#define AI_INPUT_SAMPLES 16000U
typedef enum { AI_FREE, AI_WRITING, AI_READY, AI_INFERENCE } AiBufferState;
typedef enum { EVENT_OTHER, EVENT_SNORE, EVENT_COUGH } EventClass;
typedef struct {
    uint64_t sample_position;
    EventClass event_class;
    uint8_t confidence_q8;
} SleepEvent;
void inference_task(INT stacd, void *exinf);
/* dTrig_taskのみが呼ぶ。PCMは内部バッファにコピーしてから通知。 */
ER inference_submit(const int16_t *pcm, uint32_t count, uint64_t position);
int inference_busy(void);
ER inference_reset_session(void); /* 推論停止中に次の夜の件数を0へ戻す */
#endif
