#ifndef SLEEP_DETECTION_H
#define SLEEP_DETECTION_H
#include "../audio/audio.h"
#define DETECTION_AUDIO_READY 1U
void dTrig_task(INT stacd, void *exinf);
void detection_notify_audio(void); /* audio_task専用 */
void detection_reset_session(void); /* DMA停止中にinput_taskが呼ぶ */
/* 200ms cellのRMSと適応閾値から自動トリガを判定する。 */
int detection_process_cell(const int16_t *pcm, uint32_t samples);
#endif
