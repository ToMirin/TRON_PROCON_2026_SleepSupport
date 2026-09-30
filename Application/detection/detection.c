#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX

#include "detection.h"
#include "../sleep_app_internal.h"
#include "../inference/inference.h"
#include <math.h>
#include <string.h>

/*
 * AI入力:
 *
 *   前400 ms + トリガ200 ms + 後400 ms
 * = 200 ms cell × 5
 * = 16000 samples
 */
_Static_assert(AI_INPUT_SAMPLES == AUDIO_CELL_SAMPLES * 5U,
               "AI input must be five 200 ms cells");

#define DETECTION_HISTORY_CELLS     2U
#define DETECTION_POST_CELLS        2U

#define DETECTION_INIT_CELLS        10U
#define DETECTION_RELEASE_CELLS     2U

#define DETECTION_K_TRIGGER         2.0f
#define DETECTION_K_RELEASE         1.5f
#define DETECTION_NOISE_ALPHA       0.2f

typedef enum {
    DETECTION_STATE_INIT,
    DETECTION_STATE_IDLE,
    DETECTION_STATE_ACTIVE
} DetectionState;

static int16_t cell[AUDIO_CELL_SAMPLES];
static unsigned cell_blocks;

static uint32_t previous_sequence;
static uint64_t absolute_block, cell_start;
static UINT have_sequence;

/*
 * トリガより前の2 cell = 400 msを保持する。
 *
 * history[0]: 古い200 ms
 * history[1]: 新しい200 ms
 */
static int16_t history[DETECTION_HISTORY_CELLS][AUDIO_CELL_SAMPLES];
static unsigned history_count;

/*
 * 1秒AI入力窓。
 *
 * trigger発生時:
 *   前400 ms + trigger 200 ms = 9600 samples
 *
 * その後:
 *   後200 ms × 2を追加して16000 samplesにする。
 */
static int16_t ai_window[AI_INPUT_SAMPLES];
static unsigned window_post_cells;
static UINT window_pending;
static uint64_t window_trigger_sample;

/*
 * D-08イベント検出状態。
 */
static DetectionState detection_state;
static float init_rms[DETECTION_INIT_CELLS];
static unsigned init_rms_count;
static unsigned release_count;

/*
 * Watch用。
 */
static volatile float detection_rms;
static volatile float detection_noise_floor;
static volatile float detection_trigger_threshold;
static volatile float detection_release_threshold;

static volatile uint32_t cells_processed;
static volatile uint32_t partial_cells_dropped;
static volatile uint32_t triggers_dropped;
static volatile uint32_t windows_completed;

/*
 * 200 ms cellのRMSを算出する。
 *
 * int16_t最大振幅の二乗和は32 bitを超えるため、
 * 設計書D-08に従って64 bitで累積する。
 */
static float detection_calculate_rms(const int16_t *pcm, uint32_t samples)
{
    uint64_t sum_sq = 0;

    for (uint32_t i = 0; i < samples; ++i) {
        int64_t sample = pcm[i];
        sum_sq += (uint64_t)(sample * sample);
    }

    return sqrtf((float)sum_sq / (float)samples);
}

/*
 * 10個のRMSを昇順ソートし、
 * 5番目と6番目の平均を中央値とする。
 */
static float detection_initial_noise_floor(void)
{
    float sorted[DETECTION_INIT_CELLS];

    memcpy(sorted, init_rms, sizeof(sorted));

    for (unsigned i = 1; i < DETECTION_INIT_CELLS; ++i) {
        float value = sorted[i];
        unsigned j = i;

        while (j > 0U && sorted[j - 1U] > value) {
            sorted[j] = sorted[j - 1U];
            --j;
        }

        sorted[j] = value;
    }

    return (sorted[4] + sorted[5]) * 0.5f;
}

/*
 * INIT途中のNoiseFloor候補を破棄する。
 *
 * 初期2秒は連続した有効PCMから算出したいため、
 * DMA block欠落がINIT中に発生した場合は最初から取り直す。
 */
static void detection_reset_init(void)
{
    if (detection_state == DETECTION_STATE_INIT)
        init_rms_count = 0;
}

/*
 * 過去400 msの履歴を破棄する。
 *
 * DMA block欠落後は、欠落前の音声と欠落後の音声を
 * 同じ1秒窓として連結しない。
 */
static void detection_reset_history(void)
{
    history_count = 0;

    if (window_pending) {
        window_pending = 0;
        window_post_cells = 0;
        ++triggers_dropped;
    }
}

void detection_reset_session(void)
{
    cell_blocks = 0;

    previous_sequence = 0;
    absolute_block = 0;
    cell_start = 0;
    have_sequence = 0;

    history_count = 0;

    window_post_cells = 0;
    window_pending = 0;
    window_trigger_sample = 0;

    detection_state = DETECTION_STATE_INIT;
    init_rms_count = 0;
    release_count = 0;

    detection_rms = 0.0f;
    detection_noise_floor = 0.0f;
    detection_trigger_threshold = 0.0f;
    detection_release_threshold = 0.0f;
}

int detection_process_cell(const int16_t *pcm, uint32_t samples)
{
    if (pcm == NULL || samples != AUDIO_CELL_SAMPLES)
        return 0;

    float rms = detection_calculate_rms(pcm, samples);
    detection_rms = rms;

    switch (detection_state) {
    case DETECTION_STATE_INIT:
        /*
         * startup 40 msはaudio_task側ですでに破棄済み。
         * ここではその後の有効PCM 10 cell = 2秒を使用する。
         */
        init_rms[init_rms_count++] = rms;

        if (init_rms_count == DETECTION_INIT_CELLS) {
            detection_noise_floor =
                detection_initial_noise_floor();

            detection_trigger_threshold =
                detection_noise_floor *
                DETECTION_K_TRIGGER;

            detection_release_threshold =
                detection_noise_floor *
                DETECTION_K_RELEASE;

            detection_state =
                DETECTION_STATE_IDLE;

            release_count = 0;
        }

        /*
         * NoiseFloor初期化中はAI triggerを生成しない。
         */
        return 0;

    case DETECTION_STATE_IDLE:
        detection_trigger_threshold =
            detection_noise_floor *
            DETECTION_K_TRIGGER;

        detection_release_threshold =
            detection_noise_floor *
            DETECTION_K_RELEASE;

        /*
         * IDLE → ACTIVE。
         *
         * TriggerThresholdを超えた最初のcellだけtriggerする。
         */
        if (rms > detection_trigger_threshold) {
            detection_state =
                DETECTION_STATE_ACTIVE;

            release_count = 0;

            return 1;
        }

        /*
         * triggerでないIDLE cellのみNoiseFloorをEMA更新する。
         */
        detection_noise_floor =
            (1.0f - DETECTION_NOISE_ALPHA) *
                detection_noise_floor +
            DETECTION_NOISE_ALPHA * rms;

        detection_trigger_threshold =
            detection_noise_floor *
            DETECTION_K_TRIGGER;

        detection_release_threshold =
            detection_noise_floor *
            DETECTION_K_RELEASE;

        return 0;

    case DETECTION_STATE_ACTIVE:
        detection_trigger_threshold =
            detection_noise_floor *
            DETECTION_K_TRIGGER;

        detection_release_threshold =
            detection_noise_floor *
            DETECTION_K_RELEASE;

        /*
         * ACTIVE中は新しいtriggerを生成しない。
         *
         * ReleaseThreshold未満が2 cell連続した場合のみ
         * IDLEへ戻る。
         */
        if (rms < detection_release_threshold) {
            if (++release_count >= DETECTION_RELEASE_CELLS) {
                detection_state =
                    DETECTION_STATE_IDLE;

                release_count = 0;
            }
        }
        else {
            release_count = 0;
        }

        return 0;
    }

    return 0;
}

void detection_notify_audio(void)
{
    if (sleep_app.detection_flag > 0)
        (void)tk_set_flg(sleep_app.detection_flag,
                         DETECTION_AUDIO_READY);
}

/*
 * 完成したcellを過去400 ms履歴へ追加する。
 */
static void detection_store_history(const int16_t *pcm)
{
    if (history_count < DETECTION_HISTORY_CELLS) {
        memcpy(history[history_count],
               pcm,
               sizeof(history[history_count]));

        ++history_count;
        return;
    }

    memcpy(history[0],
           history[1],
           sizeof(history[0]));

    memcpy(history[1],
           pcm,
           sizeof(history[1]));
}

/*
 * トリガcellから1秒窓生成を開始する。
 *
 * 開始時点では、
 *
 *   前400 ms + trigger 200 ms
 *
 * の600 msまでを保存する。
 */
static int detection_start_window(const int16_t *trigger_cell,
                                  uint64_t trigger_sample)
{
    if (window_pending)
        return 0;

    /*
     * セッション開始直後など、まだ前400 msが存在しない場合は
     * 1秒窓を作れない。
     */
    if (history_count < DETECTION_HISTORY_CELLS)
        return 0;

    memcpy(ai_window,
           history[0],
           AUDIO_CELL_SAMPLES * sizeof(int16_t));

    memcpy(ai_window + AUDIO_CELL_SAMPLES,
           history[1],
           AUDIO_CELL_SAMPLES * sizeof(int16_t));

    memcpy(ai_window + AUDIO_CELL_SAMPLES * 2U,
           trigger_cell,
           AUDIO_CELL_SAMPLES * sizeof(int16_t));

    window_post_cells = 0;
    window_trigger_sample = trigger_sample;
    window_pending = 1;

    return 1;
}

/*
 * trigger後の200 ms cellを追加する。
 *
 * 2 cell = 400 ms追加すると、
 *
 *   前400 + trigger200 + 後400
 *
 * の16000 samplesが完成する。
 *
 * 戻り値:
 *   0: まだ後続音声待ち
 *   1: 1秒窓完成
 */
static int detection_append_window(const int16_t *pcm)
{
    if (!window_pending)
        return 0;

    unsigned offset =
        AUDIO_CELL_SAMPLES *
        (3U + window_post_cells);

    memcpy(ai_window + offset,
           pcm,
           AUDIO_CELL_SAMPLES * sizeof(int16_t));

    ++window_post_cells;

    if (window_post_cells < DETECTION_POST_CELLS)
        return 0;

    window_pending = 0;
    window_post_cells = 0;

    ++windows_completed;

    return 1;
}

/*
 * 20 msブロックを200 msの連続区間へまとめる。
 * sequenceは32 bit周回に対応。
 */
static int detection_append(const AUDIO_AUDIO_BLOCK *block)
{
    if (have_sequence) {
        uint32_t delta =
            block->sequence - previous_sequence;

        absolute_block += delta;

        if (delta != 1U) {
            /*
             * DMA blockが欠落した場合、
             * 途中まで作っていたcellを捨てる。
             */
            if (cell_blocks != 0U)
                ++partial_cells_dropped;

            cell_blocks = 0;

            /*
             * 欠落をまたいで前後400 msを連結しない。
             */
            detection_reset_history();

            /*
             * NoiseFloor初期化中の欠落なら、
             * 連続2秒を改めて取り直す。
             */
            detection_reset_init();
        }
    }
    else {
        absolute_block = block->sequence;
        have_sequence = 1;
    }

    previous_sequence = block->sequence;

    if (cell_blocks == 0U) {
        cell_start =
            (absolute_block - 1U) *
            AUDIO_BLOCK_SAMPLES;
    }

    memcpy(cell +
               cell_blocks * AUDIO_BLOCK_SAMPLES,
           block->pcm,
           sizeof(block->pcm));

    if (++cell_blocks != AUDIO_CELL_BLOCKS)
        return 0;

    cell_blocks = 0;
    ++cells_processed;

    return 1;
}

void dTrig_task(INT stacd, void *exinf)
{
    AUDIO_AUDIO_BLOCK block;

    (void)stacd;
    (void)exinf;

    for (;;) {
        UINT events;

        ER err =
            tk_wai_flg(
                sleep_app.detection_flag,
                DETECTION_AUDIO_READY,
                TWF_ORW | TWF_BITCLR,
                &events,
                TMO_FEVR);

        if (err < E_OK)
            sleep_app_fault("dTrig wait", err);

        AUDIO_STATUS status;
        audio_get_status(&status);

        if (status.state == AUDIO_FAILED)
            sleep_app_fault("audio", E_IO);

        if (sleep_app.failed)
            continue;

        /*
         * 通知が併合されても、
         * リング内の公開済みデータを順に消費する。
         */
        for (unsigned i = 0;
             i < AUDIO_QUEUE_BLOCKS &&
             audio_read_block(&block);
             ++i) {

            if (!detection_append(&block))
                continue;

            /*
             * すでにtrigger済みなら、
             * 現在のcellは「後400 ms」の一部になる。
             */
            if (window_pending) {
                if (detection_append_window(cell)) {
                    err =
                        inference_submit(
                            ai_window,
                            AI_INPUT_SAMPLES,
                            window_trigger_sample);

                    if (err == E_OBJ)
                        ++triggers_dropped;
                    else if (err < E_OK)
                        sleep_app_fault(
                            "inference notify",
                            err);
                }
            }

            /*
             * 音量に基づく自動検出結果。
             */
            int triggered =
                detection_process_cell(
                    cell,
                    AUDIO_CELL_SAMPLES);

            if (triggered) {
                /*
                 * D-08:
                 * event_sample_positionはtrigger cell中央。
                 */
                uint64_t trigger_sample =
                    cell_start +
                    AUDIO_CELL_SAMPLES / 2U;

                /*
                 * 既存の1秒窓が後続400 ms待ちなら、
                 * 新しいtriggerは今回は捨てる。
                 */
                if (window_pending) {
                    ++triggers_dropped;
                }
                else if (!detection_start_window(
                             cell,
                             trigger_sample)) {
                    /*
                     * 前400 msがない場合など。
                     */
                    ++triggers_dropped;
                }
            }

            /*
             * trigger判定が終わってから現在cellを履歴へ入れる。
             *
             * これによりtrigger開始時のhistory[]は必ず
             * triggerより前の400 msになる。
             */
            detection_store_history(cell);
        }
    }
}

#endif