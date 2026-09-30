#ifndef SLEEP_AUDIO_H
#define SLEEP_AUDIO_H
#include <stdint.h>
#include <tk/tkernel.h>
#include "main.h"

#define AUDIO_SAMPLE_RATE 16000U
#define AUDIO_BLOCK_SAMPLES 320U
#define AUDIO_CELL_BLOCKS 10U
#define AUDIO_CELL_SAMPLES (AUDIO_BLOCK_SAMPLES * AUDIO_CELL_BLOCKS)
#define AUDIO_DMA_WORDS (AUDIO_BLOCK_SAMPLES * 2U * 2U)
#define AUDIO_QUEUE_BLOCKS 100U

typedef struct { uint32_t nonzero[2], bits[2], example[2]; } AUDIO_RAW;
typedef struct {
    int16_t pcm[AUDIO_BLOCK_SAMPLES];
    AUDIO_RAW raw;
    uint32_t clipped;
    uint32_t sequence; /* DMAブロック通番。欠落をまたぐcellの連結を防ぐ。 */
} AUDIO_AUDIO_BLOCK;
typedef enum { AUDIO_WAITING, AUDIO_RUNNING, AUDIO_STOPPED, AUDIO_FAILED } AUDIO_STATE;
typedef struct {
    AUDIO_STATE state;
    uint32_t half, full, errors, i2s_error, dma_error;
    uint32_t dropped, warmup, stalled;
    HAL_StatusTypeDef start_status, stop_status;
} AUDIO_STATUS;

ER audio_start(void);
ER audio_request_start(void);
ER audio_request_stop(void);
/* DMA停止中に次のセッションのデジタルゲインを設定する。 */
ER audio_set_gain_db(uint32_t gain_db);
void audio_get_status(AUDIO_STATUS *status);
int audio_read_block(AUDIO_AUDIO_BLOCK *block); /* 読み手はdTrig_taskのみ */
void audio_cancel_start(void); /* DMA開始要求より前の起動失敗時のみ */
void audio_task(INT stacd, void *exinf);
#endif
