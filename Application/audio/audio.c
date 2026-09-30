#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX
#include "audio.h"
#include "../detection/detection.h"

extern I2S_HandleTypeDef hi2s2; // CubeMX生成コードのI2Sハンドル。HAL_I2S_Receive_DMAで使用する。

#define DMA_EVENT_RX    1U
#define DMA_EVENT_START 2U
#define DMA_EVENT_STOP  4U
#define DMA_EVENT_ERROR 8U

/* DMAが書くのはこの配列だけ。A面20ms + B面20ms。
 * タスクのスタック外に確保し、DMA稼働中も寿命を維持する。
 * 現構成はDCACHE無効を前提とする。有効化時はキャッシュ管理が必要。
 * ダブルバッファの初期化はstatic領域のため起動時に0で行われる。
 */
static uint32_t audio_buffer[AUDIO_DMA_WORDS] __attribute__((aligned(32))); // 配列の先頭アドレスを32 bytesの倍数に配置する
/* audio_taskが生産者、dTrig_taskが消費者。未読データは上書きしない。 
* audio_pcm_queue: PCMリングバッファの実体
* 1ブロック: 20msのmono 320サンプル
* AUDIO_QUEUE_BLOCKS 個で 2000msの録音に相当.
*/
static AUDIO_AUDIO_BLOCK audio_pcm_queue[AUDIO_QUEUE_BLOCKS]; 

static volatile uint32_t audio_write_pos;
static volatile uint32_t audio_read_pos;
/* 使用数の通番と物理添字を分離。100は2の冪ではないため、
 * 32bit通番を直接%100すると周回時に未読領域と衝突する。 */
static uint32_t audio_write_index, audio_read_index;
/* デバッガのWatchでも確認可能。割り込み内ではログを出さない。 */
static volatile uint32_t audio_half_count;   // DMA受信の半分完了回数
static volatile uint32_t audio_full_count;   // DMA受信の全完了回数
static volatile uint32_t audio_completed;    // 完了通知ごとに増やす通し番号
static volatile uint32_t audio_error_count;  // DMA受信のエラー回数
static volatile uint32_t audio_i2s_error;    // I2S受信のエラーコード
static volatile uint32_t audio_dma_error;    // DMA受信のエラーコード
static volatile uint32_t audio_dropped_blocks;
static volatile uint32_t audio_warmup_blocks;
static volatile uint32_t audio_stalled;
static volatile HAL_StatusTypeDef audio_start_status = HAL_ERROR; // audio_start_status: DMA受信の開始処理が成功したかを保存する変数
static volatile HAL_StatusTypeDef audio_stop_status = HAL_ERROR;
static volatile AUDIO_STATE audio_state = AUDIO_WAITING;
static uint32_t audio_gain = 8U; /* 18 dB相当。6 dBごとに約2倍する。 */
static ID audio_task_id;
static ID audio_event_id;

/* DMA停止中にだけ呼ぶ。一晩ごとにリングと診断値を初期状態へ戻す。 */
static void audio_reset_session(void)
{
    audio_write_pos = 0;
    audio_read_pos = 0;
    audio_write_index = 0;
    audio_read_index = 0;
    audio_half_count = 0;
    audio_full_count = 0;
    audio_completed = 0;
    audio_error_count = 0;
    audio_i2s_error = 0;
    audio_dma_error = 0;
    audio_dropped_blocks = 0;
    audio_warmup_blocks = 0;
    audio_stalled = 0;
    audio_start_status = HAL_ERROR;
    audio_stop_status = HAL_ERROR;
    __DMB();
}

/* 下位24bitを符号復元 → 増幅 → 16bitへ縮小 → 飽和処理。
 * 負数の右シフトに依存せず、1倍時は従来の下位8bit切り捨てと同じ結果。
 */
static int16_t audio_pcm16(uint32_t raw, uint32_t *clipped)
{
    int32_t sample = (int32_t)(raw & 0x00ffffffU);
    if ((sample & 0x00800000) != 0) {
        sample -= 16777216;
    }
    sample *= (int32_t)audio_gain;
    int32_t pcm = sample / 256;
    if ((sample < 0) && (sample % 256 != 0)) {
        --pcm; /* Cの0方向丸めを負の無限大方向へ補正する。 */
    }
    if (pcm > 32767) {
        ++*clipped;
        return 32767;
    }
    if (pcm < -32768) {
        ++*clipped;
        return -32768;
    }
    return (int16_t)pcm;
}

// ================================================
// DMA IRQハンドラによる割り込み処理
// ================================================
//* dmaタスクへのイベント通知とエラー回数のカウント。割り込み内から呼ぶ。
static void audio_notify(UINT event)
{
    if (audio_event_id > 0) {
        if (tk_set_flg(audio_event_id, event) < E_OK) {
            ++audio_error_count;
        }
    }
}
//* DMA受信の半分完了割り込み: CubeMX生成コードのHAL_I2S_Receive_DMAで呼ばれる(IRQハンドラ内)
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        ++audio_half_count;
        ++audio_completed;
        audio_notify(DMA_EVENT_RX);
    }
}
//* DMA受信の全完了割り込み
void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        ++audio_full_count;
        ++audio_completed;
        audio_notify(DMA_EVENT_RX);
    }
}
//* DMA受信のエラー割り込み
void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        audio_i2s_error = hi2s_cb->ErrorCode;
        if (hi2s_cb->hdmarx != NULL) audio_dma_error = hi2s_cb->hdmarx->ErrorCode;
        ++audio_error_count;
        audio_notify(DMA_EVENT_ERROR);
    }
}
// ================================================


//* 指定の完了面が、現在もDMAの書き込み対象ではないことを確認する関数。 
/*
 * 通知が遅れた場合に備え、ソフトの通し番号だけでなく、未処理の完了フラグと
 * ハードの残りbyte数も確認する。コピー前後の検査を通ったブロックだけ公開する。
 * 現構成はデータキャッシュ無効を前提とする。
 */
static int audio_half_is_stable(uint32_t sequence)
{
    DMA_HandleTypeDef *dma = hi2s2.hdmarx;
    uint32_t remaining = __HAL_DMA_GET_COUNTER(dma);
    uint32_t pending = __HAL_DMA_GET_FLAG(dma, DMA_FLAG_HT | DMA_FLAG_TC);
    __DMB();
    if ((audio_completed != sequence) || (pending != 0U)) return 0;
    if ((sequence & 1U) != 0U) { /* A面完了: DMAはB面を書いている */
        return (remaining > 0U) && (remaining <= sizeof(audio_buffer) / 2U);
    }
    /* B面完了: DMAはA面を書いている */
    return (remaining > sizeof(audio_buffer) / 2U) &&
           (remaining <= sizeof(audio_buffer));
}

static void audio_store_half(uint32_t sequence)
{
    uint32_t write = audio_write_pos;
    uint32_t read = audio_read_pos;
    __DMB();

    /* 満杯時は新しいブロックを破棄。未読データは上書きしない。 */
    if ((uint32_t)(write - read) >= AUDIO_QUEUE_BLOCKS || !audio_half_is_stable(sequence)) {
        ++audio_dropped_blocks;
        return;
    }

    AUDIO_AUDIO_BLOCK *dest = &audio_pcm_queue[audio_write_index];
    UINT offset = (sequence & 1U) != 0U ? 0U : AUDIO_DMA_WORDS / 2U;
    /* 現構成ではL,Rの順。LR=GNDのため偶数wordを使用する。
     * 完了側は次の20 ms後に再利用されるので、audio_taskでコピー・PCM変換する。
     * even/oddはバッファの偶数/奇数word。RAW統計はPCM変換前の全32bitを調べる。
     */
    const volatile uint32_t *src = audio_buffer + offset;
    dest->raw = (AUDIO_RAW){0};
    dest->clipped = 0;
    dest->sequence = sequence;
    for (UINT i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        uint32_t even = src[2U * i];
        uint32_t odd = src[2U * i + 1U];
        dest->pcm[i] = audio_pcm16(even, &dest->clipped);
        dest->raw.bits[0] |= even;
        dest->raw.bits[1] |= odd;
        if (even != 0U) { ++dest->raw.nonzero[0]; dest->raw.example[0] = even; }
        if (odd != 0U) { ++dest->raw.nonzero[1]; dest->raw.example[1] = odd; }
    }
    __DMB(); /* バッファ読み取りを終えてから、上書きの有無を再確認する。 */
    if (!audio_half_is_stable(sequence)) {
        ++audio_dropped_blocks;
        return; /* 作りかけのPCMは公開しない */
    }
    __DMB(); /* PCMと診断情報を保存してから、書き込み位置を公開する。 */
    audio_write_index = (audio_write_index + 1U) % AUDIO_QUEUE_BLOCKS;
    audio_write_pos = write + 1U; // 公開済みのブロックを1つ増やす。次回の書き込みは次のブロックになる。
    if ((write + 1U - read) >= AUDIO_CELL_BLOCKS) {
        detection_notify_audio();
    }
}

//* リングバッファの公開済みブロックを読み込む。コピー終了までread_posを進めない。
int audio_read_block(AUDIO_AUDIO_BLOCK *block)
{    
    uint32_t available = audio_write_pos;    // available: 今回処理する範囲の終端（この位置自身は処理しない）
    uint32_t read = audio_read_pos;          // write_pos: 次に書き込む位置、read_pos: 次に読み込む位置
    __DMB(); /* Data Memory Barrier（書き込み位置の読み取りと、続く公開済みPCMの読み取りの順序を保証する） */
    if (read == available) return 0;

    *block = audio_pcm_queue[audio_read_index];// 
    __DMB(); /* コピー完了後に区画を解放する。 */

    audio_read_index = (audio_read_index + 1U) % AUDIO_QUEUE_BLOCKS;
    audio_read_pos = read + 1U;  // 読み込み位置を進める。次回の読み込みは次のブロックになる。
    return 1;
}

//* audio_taskの状態を取得する。DMA受信中でも呼び出せる。
void audio_get_status(AUDIO_STATUS *s)
{
    s->state = audio_state;
    __DMB();
    /* 統計表示用の概数。2カウンタの同時スナップショットではない。 */
    s->half = audio_half_count; // DMA受信の半分完了回数
    s->full = audio_full_count; // DMA受信の全完了回数
    s->errors = audio_error_count;
    s->i2s_error = audio_i2s_error;
    s->dma_error = audio_dma_error;
    s->dropped = audio_dropped_blocks;
    s->warmup = audio_warmup_blocks;
    s->stalled = audio_stalled;
    s->start_status = audio_start_status;
    s->stop_status = audio_stop_status;
}

//* I2S/DMAの設定が正しいかを確認する。誤った設定でDMA受信を開始すると、HAL_I2S_Receive_DMAが失敗するか、DMA受信が停止しない。
static int audio_configuration_valid(void)
{
    /* 未初期化のハンドルや通常DMAを誤って使用しない。 */
    if ((hi2s2.Instance != SPI2) ||
        (hi2s2.State != HAL_I2S_STATE_READY) ||
        (hi2s2.Init.Mode != I2S_MODE_MASTER_RX) ||
        (hi2s2.Init.DataFormat != I2S_DATAFORMAT_24B) ||
        (hi2s2.Init.Data24BitAlignment != I2S_DATA_24BIT_ALIGNMENT_RIGHT) ||
        (hi2s2.Init.AudioFreq != I2S_AUDIOFREQ_16K) ||
        (hi2s2.hdmarx == NULL) ||
        (hi2s2.hdmarx->InitLinkedList.LinkedListMode != DMA_LINKEDLIST_CIRCULAR) ||
        (hi2s2.hdmarx->LinkedListQueue == NULL) ||
        (hi2s2.hdmarx->LinkedListQueue->Head == NULL)) {
        return 0;
    }
    return 1;
}

//* DMA受信を停止する。HAL_I2S_DMAStopが失敗した場合はLED3を点灯してDMA受信の異常・停止失敗を表示する。
static void audio_stop_receiver(int failed)
{
    audio_stop_status = HAL_I2S_DMAStop(&hi2s2);
    HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_RESET);
    if (failed || audio_stop_status != HAL_OK) {
        // LED3を点灯してDMA受信の異常・停止失敗を表示
        HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
        __DMB();
        audio_state = AUDIO_FAILED;
        detection_notify_audio();
    } else {
        __DMB();
        audio_state = AUDIO_STOPPED;
    }
}



// ================================================
// DMA タスク処理
// I2S受信 → DMAダブルバッファ → ゲイン・16bit変換 → PCMリング
// ================================================
void audio_task(INT stacd, void *exinf)
{
    uint32_t processed = 0;
    UINT idle_waits = 0;
    (void)stacd;
    (void)exinf;

    while (1) {
        UINT events = 0;
        ER err = tk_wai_flg(audio_event_id,
                           DMA_EVENT_RX | DMA_EVENT_START | DMA_EVENT_STOP | DMA_EVENT_ERROR,
                           TWF_ORW | TWF_BITCLR, &events,
                           audio_state == AUDIO_RUNNING ? 100 : TMO_FEVR);// 100ms待機、DMA受信中はタイムアウトで停止の疑いを検出する
        if (err != E_OK && err != E_TMOUT) {
            ++audio_error_count;
            if (audio_state == AUDIO_RUNNING) audio_stop_receiver(1);
            else audio_state = AUDIO_FAILED;
            detection_notify_audio();
            tk_ext_tsk();                                            //* タスクを終了
            return;
        }
        if ((events & DMA_EVENT_START) != 0U &&
            (audio_state == AUDIO_WAITING || audio_state == AUDIO_STOPPED)) {
            audio_reset_session();
            processed = 0;
            idle_waits = 0;
            if (!audio_configuration_valid()) { // I2S/DMAの設定が不正（LED3点灯）
                HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
                audio_state = AUDIO_FAILED;
                detection_notify_audio();
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            /* FW_H5 V1.6.0の実装は24-bit時にSize * 4 bytesを転送する。
             * 引数の型はuint16_t*だが、Sizeには32-bit word数1280を渡す。
             * HAL側のコメントと実装に差があるため、FW更新時は再確認する。
             */
            // HAL_I2S_Receive_DMA: I2Sの受信をDMAで開始するための関数(HAL_OK/HAL_ERROR/HAL_BUSYを返す)
            audio_start_status = HAL_I2S_Receive_DMA(
                &hi2s2, (uint16_t *)audio_buffer, (uint16_t)AUDIO_DMA_WORDS);
            
            if (audio_start_status != HAL_OK) { // HAL_I2S_Receive_DMAが失敗（DMA受信のエラーコードを取得 & LED3点灯）
                audio_i2s_error = hi2s2.ErrorCode;
                audio_dma_error = hi2s2.hdmarx->ErrorCode;
                HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
                __DMB();
                audio_state = AUDIO_FAILED;
                detection_notify_audio();
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_SET); // DMA受信開始成功(LED1点灯)
            __DMB();
            audio_state = AUDIO_RUNNING;
        }

        if (audio_state != AUDIO_RUNNING) continue;
        if ((events & DMA_EVENT_STOP) != 0U || audio_error_count != 0U) {
            audio_stop_receiver(audio_error_count != 0U);
            if (audio_state == AUDIO_FAILED) {
                tk_ext_tsk();
                return;
            }
            /* 次の夜の開始要求を待つ。タスクとイベントフラグは維持する。 */
            continue;
        }

        uint32_t sequence = audio_completed;
        __DMB();
        uint32_t count = sequence - processed;
        /* 受信が進めばLEDで表示し、100ms待機が10回連続でタイムアウトすれば停止の疑い。 */
        if (count == 0U) { // 新しい受信がない
            if (err == E_TMOUT && ++idle_waits >= 10U) {
                audio_stalled = 1;
                audio_stop_receiver(1);
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            continue;
        }
        idle_waits = 0;
        processed = sequence;
        /* マイク安定化用の先頭40msはPCMリングへ公開しない。
         * DMAは止めず、audio_taskが完了通知を2回消費して破棄する。 */
        uint32_t warmup = 2U - audio_warmup_blocks;
        if (warmup > count) warmup = count;
        audio_warmup_blocks += warmup;
        count -= warmup;
        if (count != 0U) {
            /* 複数通知がたまった場合、既に上書きされた古い面は読まない。 */
            audio_dropped_blocks += count - 1U;
            audio_store_half(sequence);
        }
        if (audio_dropped_blocks != 0U) {
            HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
        }
        if ((sequence % 50U) == 0U) {
            HAL_GPIO_TogglePin(DBG_LED2_GPIO_Port, DBG_LED2_Pin); // DMA完了の通し番号が50の倍数ならLED2を反転
        }
    }
}
// ================================================



//* audio_taskの起動要求（フラグセット）
ER audio_request_start(void)
{
    if ((audio_state != AUDIO_WAITING && audio_state != AUDIO_STOPPED) ||
        audio_event_id <= 0) return E_OBJ;
    return tk_set_flg(audio_event_id, DMA_EVENT_START);
}

ER audio_set_gain_db(uint32_t gain_db)
{
    if (audio_state == AUDIO_RUNNING) return E_OBJ;
    if (gain_db > SLEEP_GAIN_MAX_DB ||
        (gain_db % SLEEP_GAIN_STEP_DB) != 0U) return E_PAR;
    audio_gain = 1U << (gain_db / SLEEP_GAIN_STEP_DB);
    return E_OK;
}

//* audio_taskの停止要求（フラグセット）
ER audio_request_stop(void)
{
    if (audio_state != AUDIO_RUNNING || audio_event_id <= 0) return E_OBJ;
    return tk_set_flg(audio_event_id, DMA_EVENT_STOP);
}

//* audio_taskの起動に失敗した場合の後始末
void audio_cancel_start(void)
{
    /* sleep_app_startの失敗時。DMAの受信開始を要求する前に限る。 */
    if (audio_task_id > 0) {
        (void)tk_ter_tsk(audio_task_id);
        (void)tk_del_tsk(audio_task_id);
    }
    audio_task_id = 0;
    if (audio_event_id > 0) {
        (void)tk_del_flg(audio_event_id);
    }
    audio_event_id = 0;
}

//* audio_task起動部
ER audio_start(void)
{
    if (audio_task_id > 0) return E_OBJ;
    T_CFLG flag = { .flgatr = TA_TFIFO | TA_WSGL, .iflgptn = 0 };
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3, .task = audio_task,
        .itskpri = SLEEP_AUDIO_PRIORITY, /* D-05: audio_task */
        .stksz = 2048
    };
    HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DBG_LED2_GPIO_Port, DBG_LED2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_RESET);
    audio_event_id = tk_cre_flg(&flag);
    if (audio_event_id < E_OK) return (ER)audio_event_id;
    audio_task_id = tk_cre_tsk(&task);
    if (audio_task_id < E_OK) {
        ER err = (ER)audio_task_id;
        audio_cancel_start();
        return err;
    }
    ER err = tk_sta_tsk(audio_task_id, 0);
    if (err < E_OK) audio_cancel_start();
    return err;
}

#endif
