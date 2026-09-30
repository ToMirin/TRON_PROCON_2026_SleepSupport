#ifndef SANDBOX_DMA_TEST_H
#define SANDBOX_DMA_TEST_H

#include <stdint.h>
#include <tk/tkernel.h>
#include "main.h"

/* 20 ms = mono 320 samples = L/R各320個の32-bit word。
 * A面20 ms + B面20 ms = 1280 words = 5120 bytes。
 * 開始直後のA・B（40 ms）は集計せず破棄する。
 */
#define NUM_OF_MONO_SAMPLES 320U // 20msのサンプル数
#define NUM_OF_DMA_TEST_WORDS (NUM_OF_MONO_SAMPLES * 2U * 2U) // DMAバッファのワード数
#define AUDIO_TEST_POLL_MS 10
#define DMA_TEST_QUEUE_BLOCKS 100U

/* ===== ユーザー調整設定（変更後は再ビルド・書き込み） =====
 * 処理の選択: STATS = 連続統計表示、RECORD = SW1で2秒録音・PC転送。
 * 既定値はSTATS。録音するには次の定義の末尾をDMA_TEST_MODE_RECORDへ変更。
 */
#define DMA_TEST_MODE_STATS  1
#define DMA_TEST_MODE_RECORD 2

#ifndef DMA_TEST_MODE
//#define DMA_TEST_MODE DMA_TEST_MODE_STATS
#define DMA_TEST_MODE DMA_TEST_MODE_RECORD
#endif
#if (DMA_TEST_MODE != DMA_TEST_MODE_STATS) && (DMA_TEST_MODE != DMA_TEST_MODE_RECORD)
#error "DMA_TEST_MODE must be DMA_TEST_MODE_STATS or DMA_TEST_MODE_RECORD"
#endif

/* ===== マイクのデジタルゲイン（整数1～256倍）=====
 * 1 = 増幅なし、2 = 約+6 dB、4 = 約+12 dB、8 = 約+18 dB（初期値）。
 * 小さければ上げ、clippedが増える／音が歪む場合は8→4→2→1と下げる。
 * 24bit精度のまま増幅して16bitへ変換する。統計と録音の両方に適用。
 * マイク内部の感度は変更しない。ノイズも増幅されるため実際の距離で調整。
 * RAW統計は増幅前。16bit範囲外は飽和させて符号の反転を防ぐ。
 * 最大256倍なら符号付き32bit乗算がオーバーフローしない。
 */
#ifndef DMA_TEST_GAIN
#define DMA_TEST_GAIN 8
#endif
#if (DMA_TEST_GAIN < 1) || (DMA_TEST_GAIN > 256)
#error "DMA_TEST_GAIN must be an integer from 1 to 256"
#endif

/* DMAタスクが書き、audio_taskが読む16bit PCMリングの1ブロック。
 * rawは配線確認用の診断情報。pcmはゲイン適用済みのmono 320サンプル。
 */
typedef struct {
    uint32_t nonzero[2];
    uint32_t bits[2];
    uint32_t example[2];
} DMA_TEST_RAW;

typedef struct {
    int16_t pcm[NUM_OF_MONO_SAMPLES];
    DMA_TEST_RAW raw;
    uint32_t clipped;
} DMA_TEST_AUDIO_BLOCK;

typedef enum {
    DMA_TEST_WAITING, DMA_TEST_RUNNING, DMA_TEST_STOPPED, DMA_TEST_FAILED
} DMA_TEST_STATE;

typedef struct {
    DMA_TEST_STATE state;
    uint32_t half, full, errors, i2s_error, dma_error;
    uint32_t dropped, warmup, stalled;
    HAL_StatusTypeDef start_status, stop_status;
} DMA_TEST_STATUS;

/* 開始・停止のHAL操作はDMAタスク内で行う。要求は非同期。 */
ER dma_test_start(void);
ER dma_test_request_start(void);
ER dma_test_request_stop(void);
void dma_test_get_status(DMA_TEST_STATUS *status);
/* audio_task専用。1ブロックをコピーして解放する。空なら0、取得成功なら1。 */
int dma_test_read_block(DMA_TEST_AUDIO_BLOCK *block);
/* audio_task起動失敗時だけ使用。受信開始要求前に資源を解放する。 */
void dma_test_cancel_start(void);

#endif
