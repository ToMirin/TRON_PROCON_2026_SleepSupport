#include "../sleep_app_config.h"
#if !SLEEP_APP_SANDBOX

#include "storage.h"
#include "fatfs/ff.h"
#include "../sleep_app_internal.h"
#include <tm/tmonitor.h>
#include <stdio.h>
#include <string.h>

static SleepEvent pending_event;
static volatile UINT pending;

typedef struct {
    uint64_t sample_position;
    uint8_t event_class;
    uint8_t confidence_q8;
} StoredEvent;

_Static_assert(
    sizeof(StoredEvent) == 16U,
    "StoredEvent must be 16 bytes");

_Static_assert(
    (AUDIO_SAMPLE_RATE % 1000U) == 0U,
    "Audio sample rate must be divisible by 1000");

static StoredEvent
    night_events[STORAGE_EVENT_CAPACITY];

static UINT night_count, overflow_count;
static UINT clock_ready, night_open, night_closed;

static uint64_t clock_reference;
static uint64_t configured_ms;

/*
 * 録音開始時の時刻基準。
 *
 * UI設定HH:MM + μT-Kernel経過時間を使用する。
 */
static uint64_t session_start_time_ms;

static uint16_t start_minute, stop_minute;

static ER system_time_ms(uint64_t *value)
{
    SYSTIM now;

    ER err = tk_get_tim(&now);

    if (err < E_OK)
        return err;

    *value =
        ((uint64_t)(uint32_t)now.hi << 32) |
        now.lo;

    return E_OK;
}

ER storage_set_clock(uint8_t hour,
                     uint8_t minute)
{
    if (hour >= 24U ||
        minute >= 60U)
        return E_PAR;

    if (night_open ||
        storage_busy())
        return E_OBJ;

    ER err =
        system_time_ms(
            &clock_reference);

    if (err < E_OK)
        return err;

    configured_ms =
        ((uint64_t)hour * 60U +
         minute) *
        60000U;

    clock_ready = 1;

    return E_OK;
}

ER storage_get_time_ms(uint64_t *time_ms)
{
    if (time_ms == NULL)
        return E_PAR;

    if (!clock_ready)
        return E_OBJ;

    uint64_t now;

    ER err =
        system_time_ms(&now);

    if (err < E_OK)
        return err;

    /*
     * tk_set_timで途中補正しない前提。
     * 設定確定前の稼働時間は加えない。
     */
    *time_ms =
        configured_ms +
        (now - clock_reference);

    return E_OK;
}

/*
 * sample_positionを録音開始からの経過msへ変換する。
 *
 * 16 kHzの場合:
 *   16 samples = 1 ms
 */
static uint64_t
storage_sample_to_elapsed_ms(
    uint64_t sample_position)
{
    return sample_position /
           (AUDIO_SAMPLE_RATE / 1000U);
}

/*
 * イベントのsample_positionから、
 * セッション開始時刻を基準としたイベント時刻を求める。
 */
static uint64_t
storage_event_time_ms(
    uint64_t sample_position)
{
    return session_start_time_ms +
           storage_sample_to_elapsed_ms(
               sample_position);
}

/*
 * 内部ms時刻を、0:00からの分数へ変換する。
 *
 * 0～1439:
 *   0    = 00:00
 *   1439 = 23:59
 */
static uint16_t
storage_time_to_minute(
    uint64_t time_ms)
{
    return
        (uint16_t)(
            (time_ms / 60000U) %
            1440U);
}

ER storage_begin_night(void)
{
    if (night_open ||
        night_closed ||
        storage_busy())
        return E_OBJ;

    uint64_t now;

    ER err =
        storage_get_time_ms(&now);

    if (err < E_OK)
        return err;

    /*
     * D-09:
     * セッション開始時刻を1回だけ保持する。
     *
     * UI設定時刻 + μT-Kernel経過時間。RTCは使用しない。
     */
    session_start_time_ms = now;

    night_count = 0;
    overflow_count = 0;

    start_minute =
        storage_time_to_minute(
            session_start_time_ms);

    night_open = 1;

    return E_OK;
}

ER storage_end_night(void)
{
    if (!night_open)
        return E_OBJ;

    uint64_t now;

    ER err =
        storage_get_time_ms(&now);

    if (err < E_OK)
        return err;

    stop_minute =
        storage_time_to_minute(now);

    night_open = 0;
    night_closed = 1;

    return E_OK;
}

int storage_busy(void)
{
    return pending != 0U;
}

ER storage_notify_result(
    const SleepEvent *event)
{
    if (event == NULL)
        return E_PAR;

    if (pending)
        return E_QOVR;

    pending_event = *event;

    __DMB();

    pending = 1;

    ER err =
        tk_set_flg(
            sleep_app.record_flag,
            1U);

    if (err < E_OK)
        pending = 0;

    return err;
}

ER storage_record_event(
    const SleepEvent *event)
{
    if (event == NULL ||
        (event->event_class != EVENT_OTHER &&
         event->event_class != EVENT_SNORE &&
         event->event_class != EVENT_COUGH))
        return E_PAR;

    /*
     * 停止直前の検知は、
     * 推論完了が停止後でも受領する。
     */
    if (!night_open &&
        !night_closed)
        return E_OBJ;

    /* OTHERは保存対象外。件数・overflowにも含めない。 */
    if (event->event_class == EVENT_OTHER)
        return E_OK;

    /*
     * バッファ満杯時は既存イベントを保持し、
     * 新規イベントを破棄する。
     */
    if (night_count >=
        STORAGE_EVENT_CAPACITY) {

        ++overflow_count;

        return E_QOVR;
    }

    /*
     * RAM上では時刻へ変換せず、
     * 正規キーであるsample_positionを保持する。
     */
    night_events[night_count++] =
        (StoredEvent){
            .sample_position =
                event->sample_position,

            .event_class =
                (uint8_t)
                    event->event_class,

            .confidence_q8 =
                event->confidence_q8
        };

    return E_OK;
}

/* input_taskだけが使用。FatFsの大きい作業領域は静的に置く。 */
#define STORAGE_FILE_NUMBER_MAX 99999U
/* 検証用: 1で保存後のCSVをSDから読み戻してシリアル表示。 */
#ifndef STORAGE_DEBUG_READBACK
#define STORAGE_DEBUG_READBACK 1
#endif
static FATFS log_filesystem;
static FIL log_file;
static UINT next_file_number = 1U;
/* 0: 未割当。失敗時は同じ番号を保持し、TMPを先頭から再作成する。 */
static UINT session_file_number;
static char csv_path[20];
static char tmp_path[20];

static ER write_csv_line(StorageTextWriter writer,
                         void *context,
                         uint16_t minute,
                         const char *event,
                         int has_value,
                         UINT value)
{
    char line[64];
    int length;

    if (has_value) {
        length = snprintf(line, sizeof(line),
                          "%02u:%02u,%s,%u\r\n",
                          (unsigned)(minute / 60U),
                          (unsigned)(minute % 60U),
                          event, (unsigned)value);
    } else {
        length = snprintf(line, sizeof(line),
                          "%02u:%02u,%s,\r\n",
                          (unsigned)(minute / 60U),
                          (unsigned)(minute % 60U), event);
    }
    if (length < 0 || (unsigned)length >= sizeof(line))
        return E_IO;
    return writer(line, (UINT)length, context);
}

ER storage_export_text(StorageTextWriter writer, void *context)
{
    static const char header[] = "time,event,value\r\n";
    ER err;

    if (writer == NULL)
        return E_PAR;
    if (!night_closed || sleep_app.monitoring ||
        inference_busy() || storage_busy())
        return E_OBJ;

    err = writer(header, (UINT)(sizeof(header) - 1U), context);
    if (err < E_OK)
        return err;
    err = write_csv_line(writer, context, start_minute, "START", 0, 0);
    if (err < E_OK)
        return err;

    for (UINT i = 0; i < night_count; ++i) {
        const StoredEvent *event = &night_events[i];
        const char *label;
        if (event->event_class == EVENT_OTHER)
            continue;
        if (event->event_class == EVENT_SNORE)
            label = "SNORE";
        else if (event->event_class == EVENT_COUGH)
            label = "COUGH";
        else
            return E_PAR;

        uint16_t minute = storage_time_to_minute(
            storage_event_time_ms(event->sample_position));
        err = write_csv_line(writer, context, minute, label,
                             1, event->confidence_q8);
        if (err < E_OK)
            return err;
    }
    if (overflow_count != 0U) {
        err = write_csv_line(writer, context, stop_minute,
                             "OVERFLOW", 1, overflow_count);
        if (err < E_OK)
            return err;
    }
    return write_csv_line(writer, context, stop_minute, "STOP", 0, 0);
}

static void storage_make_paths(UINT number)
{
    /* numberは1～99999。両方とも終端を含め16バイト。 */
    snprintf(csv_path, sizeof(csv_path), "0:/EVT%05u.CSV", (unsigned)number);
    snprintf(tmp_path, sizeof(tmp_path), "0:/EVT%05u.TMP", (unsigned)number);
}

/* ファイル内容は読まず、CSV/TMP両方の名前が空いている番号を選ぶ。 */
static FRESULT storage_find_free_number(void)
{
    while (next_file_number <= STORAGE_FILE_NUMBER_MAX) {
        storage_make_paths(next_file_number);
        FRESULT result = f_stat(csv_path, NULL);
        if (result == FR_OK) {
            ++next_file_number;
            continue;
        }
        if (result != FR_NO_FILE)
            return result;

        result = f_stat(tmp_path, NULL);
        if (result == FR_NO_FILE)
            return FR_OK;
        if (result != FR_OK)
            return result;
        ++next_file_number;
    }
    return FR_DENIED; /* 番号を使い切った。既存ファイルは上書きしない。 */
}

static ER storage_fatfs_writer(const char *text, UINT length, void *context)
{
    UINT written = 0;
    FRESULT result = f_write((FIL *)context, text, length, &written);
    if (result != FR_OK || written != length) {
        tm_printf((UB *)"SD log: write failed (FatFs=%d, bytes=%u/%u)\n",
                  (int)result, (unsigned)written, (unsigned)length);
        return E_IO;
    }
    return E_OK;
}

#if STORAGE_DEBUG_READBACK
/*
 * 保存済みCSVの診断表示。通常保存で使用したFatFsを一度解除し、
 * 再マウントしてSD上のファイルを読む。RAMイベントの再表示ではない。
 * この処理の失敗は、確定済みCSVの再保存・上書きを引き起こさない。
 */
static void storage_debug_readback(const char *path)
{
    char text[97];
    UINT count;
    UINT total = 0;
    FRESULT result;
    const char *stage = "unmount";
    int opened = 0;

    result = f_mount(NULL, "0:", 0);
    if (result != FR_OK)
        goto done;
    stage = "mount";
    result = f_mount(&log_filesystem, "0:", 1);
    if (result != FR_OK)
        goto done;
    stage = "open";
    result = f_open(&log_file, path, FA_READ);
    if (result != FR_OK)
        goto done;
    opened = 1;

    tm_printf((UB *)"SD readback BEGIN: %s\n", path);
    stage = "read";
    for (;;) {
        result = f_read(&log_file, text, sizeof(text) - 1U, &count);
        if (result != FR_OK)
            goto done;
        if (count == 0U)
            break;
        text[count] = '\0';
        tm_putstring((UB *)text);
        total += count;
    }

done:
    if (opened) {
        FRESULT close_result = f_close(&log_file);
        if (result == FR_OK) {
            result = close_result;
            stage = "close";
        }
    }
    FRESULT unmount_result = f_mount(NULL, "0:", 0);
    if (result == FR_OK && unmount_result != FR_OK) {
        result = unmount_result;
        stage = "unmount";
    }
    if (result == FR_OK) {
        tm_printf((UB *)"SD readback END: %u bytes\n", (unsigned)total);
    } else {
        tm_printf((UB *)"\nSD readback FAILED: %s (FatFs=%d)\n",
                  stage, (int)result);
        tm_putstring((UB *)"SD log: CSV already saved; readback incomplete.\n");
    }
}
#endif

ER storage_save_night(void)
{
    FRESULT result;
    ER err = E_IO;
    const char *stage = "mount";
    int opened = 0;

    if (!night_closed || sleep_app.monitoring ||
        inference_busy() || storage_busy())
        return E_OBJ;

    result = f_mount(&log_filesystem, "0:", 1);
    if (result != FR_OK)
        goto done;

    if (session_file_number == 0U) {
        stage = "find free number";
        result = storage_find_free_number();
        if (result != FR_OK)
            goto done;
        stage = "create TMP";
        /* 存在確認後もCREATE_NEWで既存TMPの上書きを防ぐ。 */
        result = f_open(&log_file, tmp_path, FA_CREATE_NEW | FA_WRITE);
        if (result != FR_OK)
            goto done;
        session_file_number = next_file_number;
    } else {
        storage_make_paths(session_file_number);
        stage = "check CSV before retry";
        result = f_stat(csv_path, NULL);
        if (result == FR_OK) {
            result = FR_EXIST;
            goto done;
        }
        if (result != FR_NO_FILE)
            goto done;
        stage = "recreate TMP";
        result = f_open(&log_file, tmp_path, FA_CREATE_ALWAYS | FA_WRITE);
        if (result != FR_OK)
            goto done;
    }
    opened = 1;
    stage = "export CSV";
    err = storage_export_text(storage_fatfs_writer, &log_file);
    if (err < E_OK)
        goto done;

    stage = "close TMP";
    result = f_close(&log_file); /* データとディレクトリエントリを同期 */
    opened = 0;
    if (result != FR_OK) {
        err = E_IO;
        goto done;
    }

    stage = "rename TMP to CSV";
    result = f_rename(tmp_path, csv_path); /* 既存CSVは上書きしない */
    if (result != FR_OK) {
        err = E_IO;
        goto done;
    }

    tm_printf((UB *)"SD log: saved %s\n", csv_path);
    next_file_number = session_file_number + 1U;
    session_file_number = 0U;
    night_closed = 0U;
    err = E_OK;

#if STORAGE_DEBUG_READBACK
    /* 保存成功を確定後、診断として読み戻す。失敗しても再保存しない。 */
    storage_debug_readback(csv_path);
#endif

done:
    if (opened) {
        FRESULT close_result = f_close(&log_file);
        if (close_result != FR_OK)
            tm_printf((UB *)"SD log: cleanup close failed (FatFs=%d)\n",
                      (int)close_result);
    }
    (void)f_mount(NULL, "0:", 0);
    if (err < E_OK)
        tm_printf((UB *)"SD log: %s failed (FatFs=%d, error=%d)\n",
                  stage, (int)result, (int)err);
    /* 失敗時はnight_closed・RAMイベント・割当済み番号を保持する。 */
    return err;
}

void record_task(INT stacd,
                 void *exinf)
{
    (void)stacd;
    (void)exinf;

    for (;;) {
        UINT events;

        ER err =
            tk_wai_flg(
                sleep_app.record_flag,
                1U,
                TWF_ORW |
                    TWF_BITCLR,
                &events,
                TMO_FEVR);

        if (err < E_OK)
            sleep_app_fault(
                "record wait",
                err);

        err =
            storage_record_event(
                &pending_event);

        __DMB();

        pending = 0;

        if (err == E_QOVR) {
            if (overflow_count == 1U)
                tm_putstring(
                    (UB *)
                    "storage: event capacity exceeded.\n");
        }
        else if (err < E_OK) {
            sleep_app_fault(
                "record event",
                err);
        }
    }
}

#endif