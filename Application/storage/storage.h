#ifndef SLEEP_STORAGE_H
#define SLEEP_STORAGE_H
#include "../inference/inference.h"
#define STORAGE_EVENT_CAPACITY 1024U
/* input_task専用。設定確定時に時計の基準を採取する。 */
ER storage_set_clock(uint8_t hour, uint8_t minute);
ER storage_get_time_ms(uint64_t *time_ms);
ER storage_begin_night(void);
ER storage_end_night(void);
void record_task(INT stacd, void *exinf);
ER storage_notify_result(const SleepEvent *event);
int storage_busy(void); /* RAMへの追加完了まで1 */
ER storage_record_event(const SleepEvent *event);
/* CSV: time,event,value / CRLF。停止・推論・記録完了後のみ使用。
 * writerは全量受領時にE_OKを返す。valueはconfidence_q8またはoverflow件数。 */
typedef ER (*StorageTextWriter)(const char *text, UINT length, void *context);
ER storage_export_text(StorageTextWriter writer, void *context);
/* input_task専用。EVTnnnnn.TMPへ保存後、CSVへ改名して成功とする。
 * 失敗時はRAMと割当済み番号を保持し、同じTMPで再試行する。 */
ER storage_save_night(void);
#endif
