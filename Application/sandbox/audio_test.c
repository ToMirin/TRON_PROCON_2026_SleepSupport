/*
 * audio_task: PCMリングの消費、統計表示、2秒録音、PC転送。
 * DMAバッファやHALの開始・停止を直接操作せず、dma_test.hのAPIを使う。
 * app_main.cからdma_test.cと一緒に取り込む。単独ではビルドしない。
 */
#include "dma_test.h"
#include <tm/tmonitor.h>

static ID audio_task_id;
static DMA_TEST_AUDIO_BLOCK audio_block; /* タスクスタック外の読み出し先 */

//! ==================================================
//! DMA_TEST_MODE_RECORDのみの設定
//! ==================================================
#if DMA_TEST_MODE == DMA_TEST_MODE_RECORD
#define AUDIO_RECORD_SAMPLES 32000U
_Static_assert(AUDIO_RECORD_SAMPLES % NUM_OF_MONO_SAMPLES == 0,
               "Recording length must contain whole PCM blocks");
_Static_assert(AUDIO_RECORD_SAMPLES % 64U == 0,
               "Serial transfer requires complete 64-sample lines");
static int16_t audio_recording[AUDIO_RECORD_SAMPLES];
static uint32_t audio_recorded;

/* 115200bpsでは32,000 bytes/sの音声を連続転送できない。
 * 必ずDMA停止確認後に送る。改行変換のあるtm_putcharにも安全な16進形式。
 * FNV-1aは各sampleの下位byte、上位byteの順で計算する。
 * PCM転送形式はtools/capture_mic.pyと共通。
 */
static void audio_send_recording(void)
{
    uint32_t hash = 2166136261U;
    tm_putstring((UB *)"PCM_BEGIN 16000 32000\n");
    for (UINT i = 0; i < AUDIO_RECORD_SAMPLES; ++i) {
        uint16_t sample = (uint16_t)audio_recording[i];
        if ((i % 64U) == 0U) tm_printf((UB *)"PCM %lu ", (unsigned long)i);
        tm_printf((UB *)"%04x", (unsigned int)sample);
        hash = (hash ^ (sample & 0xffU)) * 16777619U;
        hash = (hash ^ (sample >> 8)) * 16777619U;
        if ((i % 64U) == 63U) {
            tm_putchar('\n');
            tk_dly_tsk(1); /* 転送中も他のタスクへ実行機会を渡す。 */
        }
    }
    tm_printf((UB *)"PCM_END %08lx\n", (unsigned long)hash);
}
#endif
//! ==================================================

static void audio_task(INT stacd, void *exinf)
{
    DMA_TEST_STATUS status;
    uint32_t clipped = 0;
#if DMA_TEST_MODE == DMA_TEST_MODE_STATS
    UINT polls = 0;
    int32_t pcm_min = 32767, pcm_max = -32768;
    uint32_t pcm_samples = 0;
    DMA_TEST_RAW raw_stats = {0};
#endif
    (void)stacd;
    (void)exinf;

#if DMA_TEST_MODE == DMA_TEST_MODE_RECORD
    audio_recorded = 0;
    tm_printf((UB *)"AUDIO_TEST: mode=RECORD, gain=%d\n", DMA_TEST_GAIN);
    tm_putstring((UB *)"PCM_READY: start PC receiver, then press SW1_USER; speak for 2 seconds.\n");
    /* 押しっぱなしの起動を避け、離した後の押下を待つ。SW1はActive High。 */
    while (HAL_GPIO_ReadPin(SW1_USER_GPIO_Port, SW1_USER_Pin) == GPIO_PIN_SET) tk_dly_tsk(10);
    while (HAL_GPIO_ReadPin(SW1_USER_GPIO_Port, SW1_USER_Pin) == GPIO_PIN_RESET) tk_dly_tsk(10);
#else
    tm_printf((UB *)"AUDIO_TEST: mode=STATS, gain=%d\n", DMA_TEST_GAIN);
#endif
    if (dma_test_request_start() < E_OK) goto failed;
    do {
        tk_dly_tsk(AUDIO_TEST_POLL_MS);
        dma_test_get_status(&status);
    } while (status.state == DMA_TEST_WAITING);
    if (status.state != DMA_TEST_RUNNING) goto failed;

    for (;;) {
        /* 公開済みPCMを取り出すのはaudio_taskだけ。変換済みなので増幅しない。 */
        while (dma_test_read_block(&audio_block)) {
            clipped += audio_block.clipped;
#if DMA_TEST_MODE == DMA_TEST_MODE_RECORD
            for (UINT i = 0; i < NUM_OF_MONO_SAMPLES; ++i) {
                audio_recording[audio_recorded++] = audio_block.pcm[i];
            }
            if (audio_recorded == AUDIO_RECORD_SAMPLES) break;
#else
            for (UINT ch = 0; ch < 2U; ++ch) {
                raw_stats.nonzero[ch] += audio_block.raw.nonzero[ch];
                raw_stats.bits[ch] |= audio_block.raw.bits[ch];
                if (audio_block.raw.nonzero[ch] != 0U) raw_stats.example[ch] = audio_block.raw.example[ch];
            }
            for (UINT i = 0; i < NUM_OF_MONO_SAMPLES; ++i) {
                int32_t sample = audio_block.pcm[i];
                if (sample < pcm_min) pcm_min = sample;
                if (sample > pcm_max) pcm_max = sample;
                ++pcm_samples;
            }
#endif
        }
        dma_test_get_status(&status);
#if DMA_TEST_MODE == DMA_TEST_MODE_RECORD
        /* 欠落した音声を正常な録音として出力しない。停止後の転送なので、
         * シリアル出力中にPCMリングがあふれることもない。 */
        if (audio_recorded == AUDIO_RECORD_SAMPLES || status.dropped != 0U ||
            status.errors != 0U || status.state != DMA_TEST_RUNNING) {
            if (status.state == DMA_TEST_RUNNING) {
                if (dma_test_request_stop() < E_OK) goto failed;
                do {
                    tk_dly_tsk(AUDIO_TEST_POLL_MS);
                    dma_test_get_status(&status);
                } while (status.state == DMA_TEST_RUNNING);
            }
            if (status.state != DMA_TEST_STOPPED || status.stop_status != HAL_OK ||
                audio_recorded != AUDIO_RECORD_SAMPLES || status.dropped != 0U ||
                status.errors != 0U || status.stalled != 0U) goto failed;
            tm_printf((UB *)"MIC_RECORD: samples=%lu gain=%d clipped=%lu\n",
                      (unsigned long)audio_recorded, DMA_TEST_GAIN, (unsigned long)clipped);
            audio_send_recording();
            tm_putstring((UB *)"PCM_DONE: reset board for another recording.\n");
            tk_ext_tsk();
            return;
        }
#else
        if (status.state != DMA_TEST_RUNNING) goto failed;
        if (++polls % 100U == 0U) {
            tm_printf((UB *)"DMA_TEST: half=%lu full=%lu errors=%lu stalled=%lu i2s=%lu dma=%lu\n",
                      (unsigned long)status.half, (unsigned long)status.full,
                      (unsigned long)status.errors, (unsigned long)status.stalled,
                      (unsigned long)status.i2s_error, (unsigned long)status.dma_error);
            if (pcm_samples != 0U) {
                tm_printf((UB *)"MIC: samples=%lu min=%ld max=%ld p2p=%ld dropped=%lu warmup=%lu/2 gain=%d clipped=%lu\n",
                          (unsigned long)pcm_samples, (long)pcm_min, (long)pcm_max,
                          (long)(pcm_max-pcm_min), (unsigned long)status.dropped,
                          (unsigned long)status.warmup, DMA_TEST_GAIN, (unsigned long)clipped);
            } else {
                tm_printf((UB *)"MIC: no samples, dropped=%lu warmup=%lu/2\n",
                          (unsigned long)status.dropped, (unsigned long)status.warmup);
            }
            /* nzは非ゼロword数、orは期間内の全wordのビットOR、
             * lastは最後に見つかった非ゼロword（なければ0）。 */
            tm_printf((UB *)"RAW: even nz=%lu or=%08lx last=%08lx | odd nz=%lu or=%08lx last=%08lx\n",
                      (unsigned long)raw_stats.nonzero[0], (unsigned long)raw_stats.bits[0],
                      (unsigned long)raw_stats.example[0], (unsigned long)raw_stats.nonzero[1],
                      (unsigned long)raw_stats.bits[1], (unsigned long)raw_stats.example[1]);
            raw_stats = (DMA_TEST_RAW){0};
            pcm_min = 32767; pcm_max = -32768; pcm_samples = 0; clipped = 0;
        }
#endif
        tk_dly_tsk(AUDIO_TEST_POLL_MS);
    }
failed:
    dma_test_get_status(&status);
    HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
#if DMA_TEST_MODE == DMA_TEST_MODE_RECORD
    tm_putstring((UB *)"PCM_ABORT: capture/stop failed; reset and retry.\n");
#else
    tm_printf((UB *)"DMA_TEST: failed start=%d stop=%d errors=%lu stalled=%lu i2s=%lu dma=%lu\n",
              (int)status.start_status, (int)status.stop_status,
              (unsigned long)status.errors, (unsigned long)status.stalled,
              (unsigned long)status.i2s_error, (unsigned long)status.dma_error);
#endif
    tk_ext_tsk();
}

/* 両タスクを起動。audio_taskだけではDMA受信を直接開始しない。 */
static ER audio_test_start(void)
{
    if (audio_task_id > 0) return E_OBJ;
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3, .task = audio_task,
        .itskpri = 10, //! タスク優先度：10
        .stksz = 2048
    };
    audio_task_id = tk_cre_tsk(&task);
    if (audio_task_id < E_OK) return (ER)audio_task_id;
    ER err = dma_test_start();
    if (err >= E_OK) {
        err = tk_sta_tsk(audio_task_id, 0);
        if (err < E_OK) dma_test_cancel_start();
    }
    if (err < E_OK) {
        (void)tk_del_tsk(audio_task_id);
        audio_task_id = 0;
    }
    return err;
}
