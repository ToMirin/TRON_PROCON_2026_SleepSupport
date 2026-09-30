/*
 * DMAタスク: I2S受信 → DMAダブルバッファ → ゲイン・16bit変換 → PCMリング。
 * 統計表示、録音保存、PC転送はaudio_test.cが担当する。
 * 設定はdma_test.h冒頭。CubeMXの生成コードは変更しない。
 * ICS-43434: LR=GND、WS=PB12、SCK=PB13、SD=PB14。
 * app_main.cから取り込む。sandbox内のCファイルは独立ビルドしない。
 */
#include "dma_test.h"

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
static uint32_t dma_test_buffer[NUM_OF_DMA_TEST_WORDS] __attribute__((aligned(32))); // 配列の先頭アドレスを32 bytesの倍数に配置する
/* DMAタスクが生産者、audio_taskが消費者。未読データは上書きしない。
* dma_test_pcm_queue: PCMリングバッファの実体
* 1ブロック: 20msのmono 320サンプル
* DMA_TEST_QUEUE_BLOCKS 個で 2000msの録音に相当.
*/
static DMA_TEST_AUDIO_BLOCK dma_test_pcm_queue[DMA_TEST_QUEUE_BLOCKS];

static volatile uint32_t dma_test_write_pos;
static volatile uint32_t dma_test_read_pos;
/* デバッガのWatchでも確認可能。割り込み内ではログを出さない。 */
static volatile uint32_t dma_test_half_count;   // DMA受信の半分完了回数
static volatile uint32_t dma_test_full_count;   // DMA受信の全完了回数
static volatile uint32_t dma_test_completed;    // 完了通知ごとに増やす通し番号
static volatile uint32_t dma_test_error_count;  // DMA受信のエラー回数
static volatile uint32_t dma_test_i2s_error;    // I2S受信のエラーコード
static volatile uint32_t dma_test_dma_error;    // DMA受信のエラーコード
static volatile uint32_t dma_test_dropped_blocks;
static volatile uint32_t dma_test_warmup_blocks;
static volatile uint32_t dma_test_stalled;
static volatile HAL_StatusTypeDef dma_test_start_status = HAL_ERROR; // dma_test_start_status: DMA受信の開始処理が成功したかを保存する変数
static volatile HAL_StatusTypeDef dma_test_stop_status = HAL_ERROR;
static volatile DMA_TEST_STATE dma_test_state = DMA_TEST_WAITING;
static ID dma_test_task_id;
static ID dma_test_event_id;

/* 下位24bitを符号復元 → 増幅 → 16bitへ縮小 → 飽和処理。
 * 負数の右シフトに依存せず、1倍時は従来の下位8bit切り捨てと同じ結果。
 */
static int16_t dma_test_pcm16(uint32_t raw, uint32_t *clipped)
{
    int32_t sample = (int32_t)(raw & 0x00ffffffU);
    if ((sample & 0x00800000) != 0) {
        sample -= 16777216;
    }
    sample *= DMA_TEST_GAIN;
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
static void dma_test_notify(UINT event)
{
    if (dma_test_event_id > 0) {
        if (tk_set_flg(dma_test_event_id, event) < E_OK) {
            ++dma_test_error_count;
        }
    }
}
//* DMA受信の半分完了割り込み: CubeMX生成コードのHAL_I2S_Receive_DMAで呼ばれる(IRQハンドラ内)
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        ++dma_test_half_count;
        ++dma_test_completed;
        dma_test_notify(DMA_EVENT_RX);
    }
}
//* DMA受信の全完了割り込み
void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        ++dma_test_full_count;
        ++dma_test_completed;
        dma_test_notify(DMA_EVENT_RX);
    }
}
//* DMA受信のエラー割り込み
void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s_cb)
{
    if (hi2s_cb == &hi2s2) {
        dma_test_i2s_error = hi2s_cb->ErrorCode;
        if (hi2s_cb->hdmarx != NULL) dma_test_dma_error = hi2s_cb->hdmarx->ErrorCode;
        ++dma_test_error_count;
        dma_test_notify(DMA_EVENT_ERROR);
    }
}
// ================================================


//* 指定の完了面が、現在もDMAの書き込み対象ではないことを確認する関数。
/*
 * 通知が遅れた場合に備え、ソフトの通し番号だけでなく、未処理の完了フラグと
 * ハードの残りbyte数も確認する。コピー前後の検査を通ったブロックだけ公開する。
 * 現構成はデータキャッシュ無効を前提とする。
 */
static int dma_test_half_is_stable(uint32_t sequence)
{
    DMA_HandleTypeDef *dma = hi2s2.hdmarx;
    uint32_t remaining = __HAL_DMA_GET_COUNTER(dma);
    uint32_t pending = __HAL_DMA_GET_FLAG(dma, DMA_FLAG_HT | DMA_FLAG_TC);
    __DMB();
    if ((dma_test_completed != sequence) || (pending != 0U)) return 0;
    if ((sequence & 1U) != 0U) { /* A面完了: DMAはB面を書いている */
        return (remaining > 0U) && (remaining <= sizeof(dma_test_buffer) / 2U);
    }
    /* B面完了: DMAはA面を書いている */
    return (remaining > sizeof(dma_test_buffer) / 2U) &&
           (remaining <= sizeof(dma_test_buffer));
}

static void dma_test_store_half(uint32_t sequence)
{
    uint32_t write = dma_test_write_pos;
    uint32_t read = dma_test_read_pos;
    __DMB();

    /* 書き込み位置が読み込み位置に追いついた(リングバッファが満杯)→リングバッファ内の古いブロックを破棄する。 */
    if ((uint32_t)(write - read) >= DMA_TEST_QUEUE_BLOCKS || !dma_test_half_is_stable(sequence)) {
        ++dma_test_dropped_blocks;
        return;
    }

    DMA_TEST_AUDIO_BLOCK *dest = &dma_test_pcm_queue[write % DMA_TEST_QUEUE_BLOCKS];
    UINT offset = (sequence & 1U) != 0U ? 0U : NUM_OF_DMA_TEST_WORDS / 2U;
    /* 現構成ではL,Rの順。LR=GNDのため偶数wordを使用する。
     * 完了側は次の20 ms後に再利用されるので、DMAタスクでコピー・PCM変換する。
     * even/oddはバッファの偶数/奇数word。RAW統計はPCM変換前の全32bitを調べる。
     */
    const volatile uint32_t *src = dma_test_buffer + offset;
    dest->raw = (DMA_TEST_RAW){0};
    dest->clipped = 0;
    for (UINT i = 0; i < NUM_OF_MONO_SAMPLES; ++i) {
        uint32_t even = src[2U * i];
        uint32_t odd = src[2U * i + 1U];
        dest->pcm[i] = dma_test_pcm16(even, &dest->clipped);
        dest->raw.bits[0] |= even;
        dest->raw.bits[1] |= odd;
        if (even != 0U) { ++dest->raw.nonzero[0]; dest->raw.example[0] = even; }
        if (odd != 0U) { ++dest->raw.nonzero[1]; dest->raw.example[1] = odd; }
    }
    __DMB(); /* バッファ読み取りを終えてから、上書きの有無を再確認する。 */
    if (!dma_test_half_is_stable(sequence)) {
        ++dma_test_dropped_blocks;
        return; /* 作りかけのPCMは公開しない */
    }
    __DMB(); /* PCMと診断情報を保存してから、書き込み位置を公開する。 */
    dma_test_write_pos = write + 1U; // 公開済みのブロックを1つ増やす。次回の書き込みは次のブロックになる。
}

//* リングバッファの公開済みブロックを読み込む。コピー終了までread_posを進めない。
int dma_test_read_block(DMA_TEST_AUDIO_BLOCK *block)
{
    uint32_t available = dma_test_write_pos;    // available: 今回処理する範囲の終端（この位置自身は処理しない）
    uint32_t read = dma_test_read_pos;          // write_pos: 次に書き込む位置、read_pos: 次に読み込む位置
    __DMB(); /* Data Memory Barrier（書き込み位置の読み取りと、続く公開済みPCMの読み取りの順序を保証する） */
    if (read == available) return 0;

    *block = dma_test_pcm_queue[read % DMA_TEST_QUEUE_BLOCKS];//
    __DMB(); /* コピー完了後に区画を解放する。 */

    dma_test_read_pos = read + 1U;  // 読み込み位置を進める。次回の読み込みは次のブロックになる。
    return 1;
}

//* DMAタスクの状態を取得する。DMA受信中でも呼び出せる。
void dma_test_get_status(DMA_TEST_STATUS *s)
{
    s->state = dma_test_state;
    __DMB();
    /* 統計表示用の概数。2カウンタの同時スナップショットではない。 */
    s->half = dma_test_half_count; // DMA受信の半分完了回数
    s->full = dma_test_full_count; // DMA受信の全完了回数
    s->errors = dma_test_error_count;
    s->i2s_error = dma_test_i2s_error;
    s->dma_error = dma_test_dma_error;
    s->dropped = dma_test_dropped_blocks;
    s->warmup = dma_test_warmup_blocks;
    s->stalled = dma_test_stalled;
    s->start_status = dma_test_start_status;
    s->stop_status = dma_test_stop_status;
}

//* I2S/DMAの設定が正しいかを確認する。誤った設定でDMA受信を開始すると、HAL_I2S_Receive_DMAが失敗するか、DMA受信が停止しない。
static int dma_test_configuration_valid(void)
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
static void dma_test_stop_receiver(int failed)
{
    dma_test_stop_status = HAL_I2S_DMAStop(&hi2s2);
    if (failed || dma_test_stop_status != HAL_OK) {
        // LED3を点灯してDMA受信の異常・停止失敗を表示
        HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
        __DMB();
        dma_test_state = DMA_TEST_FAILED;
    } else {
        __DMB();
        dma_test_state = DMA_TEST_STOPPED;
    }
}



// ================================================
// DMA タスク処理
// I2S受信 → DMAダブルバッファ → ゲイン・16bit変換 → PCMリング
// ================================================
static void dma_test_task(INT stacd, void *exinf)
{
    uint32_t processed = 0;
    UINT idle_waits = 0;
    (void)stacd;
    (void)exinf;

    while (1) {
        UINT events = 0;
        ER err = tk_wai_flg(dma_test_event_id,
                           DMA_EVENT_RX | DMA_EVENT_START | DMA_EVENT_STOP | DMA_EVENT_ERROR,
                           TWF_ORW | TWF_BITCLR, &events,
                           dma_test_state == DMA_TEST_RUNNING ? 100 : TMO_FEVR);// 100ms待機、DMA受信中はタイムアウトで停止の疑いを検出する
        if (err != E_OK && err != E_TMOUT) {
            ++dma_test_error_count;
            if (dma_test_state == DMA_TEST_RUNNING) dma_test_stop_receiver(1);
            else dma_test_state = DMA_TEST_FAILED;
            tk_ext_tsk();                                            //* タスクを終了
            return;
        }
        if ((events & DMA_EVENT_START) != 0U && dma_test_state == DMA_TEST_WAITING) {
            if (!dma_test_configuration_valid()) { // I2S/DMAの設定が不正（LED3点灯）
                HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
                dma_test_state = DMA_TEST_FAILED;
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            /* FW_H5 V1.6.0の実装は24-bit時にSize * 4 bytesを転送する。
             * 引数の型はuint16_t*だが、Sizeには32-bit word数1280を渡す。
             * HAL側のコメントと実装に差があるため、FW更新時は再確認する。
             */
            // HAL_I2S_Receive_DMA: I2Sの受信をDMAで開始するための関数(HAL_OK/HAL_ERROR/HAL_BUSYを返す)
            dma_test_start_status = HAL_I2S_Receive_DMA(
                &hi2s2, (uint16_t *)dma_test_buffer, (uint16_t)NUM_OF_DMA_TEST_WORDS);

            if (dma_test_start_status != HAL_OK) { // HAL_I2S_Receive_DMAが失敗（DMA受信のエラーコードを取得 & LED3点灯）
                dma_test_i2s_error = hi2s2.ErrorCode;
                dma_test_dma_error = hi2s2.hdmarx->ErrorCode;
                HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
                __DMB();
                dma_test_state = DMA_TEST_FAILED;
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_SET); // DMA受信開始成功(LED1点灯)
            __DMB();
            dma_test_state = DMA_TEST_RUNNING;
        }

        if (dma_test_state != DMA_TEST_RUNNING) continue;
        if ((events & DMA_EVENT_STOP) != 0U || dma_test_error_count != 0U) {
            dma_test_stop_receiver(dma_test_error_count != 0U);
            tk_ext_tsk();                                            //* タスクを終了
            return;
        }

        uint32_t sequence = dma_test_completed;
        __DMB();
        uint32_t count = sequence - processed;
        /* 受信が進めばLEDで表示し、100ms待機が10回連続でタイムアウトすれば停止の疑い。 */
        if (count == 0U) { // 新しい受信がない
            if (err == E_TMOUT && ++idle_waits >= 10U) {
                dma_test_stalled = 1;
                dma_test_stop_receiver(1);
                tk_ext_tsk();                                       //* タスクを終了
                return;
            }
            continue;
        }
        idle_waits = 0;
        processed = sequence;
        /* 先頭2ブロックの破棄は既存テストの動作を維持。 */
        uint32_t warmup = 2U - dma_test_warmup_blocks;
        if (warmup > count) warmup = count;
        dma_test_warmup_blocks += warmup;
        count -= warmup;
        if (count != 0U) {
            /* 複数通知がたまった場合、既に上書きされた古い面は読まない。 */
            dma_test_dropped_blocks += count - 1U;
            dma_test_store_half(sequence);
        }
        if (dma_test_dropped_blocks != 0U) {
            HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_SET);
        }
        if ((sequence % 50U) == 0U) {
            HAL_GPIO_TogglePin(DBG_LED2_GPIO_Port, DBG_LED2_Pin); // DMA完了の通し番号が50の倍数ならLED2を反転
        }
    }
}
// ================================================



//* DMAタスクの起動要求（フラグセット）
ER dma_test_request_start(void)
{
    if (dma_test_state != DMA_TEST_WAITING || dma_test_event_id <= 0) return E_OBJ;
    return tk_set_flg(dma_test_event_id, DMA_EVENT_START);
}

//* DMAタスクの停止要求（フラグセット）
ER dma_test_request_stop(void)
{
    if (dma_test_state != DMA_TEST_RUNNING || dma_test_event_id <= 0) return E_OBJ;
    return tk_set_flg(dma_test_event_id, DMA_EVENT_STOP);
}

//* DMAタスクの起動に失敗した場合の後始末
void dma_test_cancel_start(void)
{
    /* 受信を要求するaudio_taskを起動できなかった場合の後始末。 */
    if (dma_test_task_id > 0) {
        (void)tk_ter_tsk(dma_test_task_id);
        (void)tk_del_tsk(dma_test_task_id);
    }
    dma_test_task_id = 0;
    if (dma_test_event_id > 0) {
        (void)tk_del_flg(dma_test_event_id);
    }
    dma_test_event_id = 0;
}

//* DMAタスク起動部
ER dma_test_start(void)
{
    if (dma_test_task_id > 0) return E_OBJ;
    T_CFLG flag = { .flgatr = TA_TFIFO | TA_WSGL, .iflgptn = 0 };
    T_CTSK task = {
        .tskatr = TA_HLNG | TA_RNG3, .task = dma_test_task,
        .itskpri = 9, //! タスク優先度：9（audio_taskの10より優先し、次の20ms以内にPCMへ格納）
        .stksz = 2048
    };
    HAL_GPIO_WritePin(DBG_LED1_GPIO_Port, DBG_LED1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DBG_LED2_GPIO_Port, DBG_LED2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DBG_LED3_GPIO_Port, DBG_LED3_Pin, GPIO_PIN_RESET);
    dma_test_event_id = tk_cre_flg(&flag);
    if (dma_test_event_id < E_OK) return (ER)dma_test_event_id;
    dma_test_task_id = tk_cre_tsk(&task);
    if (dma_test_task_id < E_OK) {
        ER err = (ER)dma_test_task_id;
        dma_test_cancel_start();
        return err;
    }
    ER err = tk_sta_tsk(dma_test_task_id, 0);
    if (err < E_OK) dma_test_cancel_start();
    return err;
}
