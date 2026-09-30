/*
 * EVT-04A:
 * sample_positionから録音開始後の経過時間へ変換する式を確認する。
 *
 * D-09:
 *
 *   elapsed_ms
 *     = sample_position * 1000 / sample_rate
 *
 * 現在は16 kHzなので、
 *
 *   16 samples = 1 ms
 *
 * となる。
 *
 * app_main.cからincludeして使用する。
 * sandbox内のCファイルは独立ビルドしない。
 */

#include "../audio/audio.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <stdint.h>

typedef struct {
    uint64_t sample_position;
    uint64_t expected_ms;
} TimeTestCase;

/*
 * production側の
 * storage_sample_to_elapsed_ms()
 * と同じ計算式。
 */
static uint64_t time_test_sample_to_elapsed_ms(
    uint64_t sample_position)
{
    return sample_position /
           (AUDIO_SAMPLE_RATE / 1000U);
}

static int time_test_run_case(
    unsigned index,
    const TimeTestCase *test)
{
    uint64_t actual_ms =
        time_test_sample_to_elapsed_ms(
            test->sample_position);

    /*
     * tm_printfは%lluを扱えないため、
     * 今回のテスト範囲では32bitに収まる値だけを
     * unsignedへ変換して表示する。
     *
     * 計算自体はuint64_tのまま行う。
     */
    tm_printf(
        (UB *)
        "TIME_TEST[%u]: sample=%u expected=%u ms actual=%u ms -> %s\n",
        index,
        (unsigned)test->sample_position,
        (unsigned)test->expected_ms,
        (unsigned)actual_ms,
        actual_ms == test->expected_ms ?
            "PASS" : "FAIL");

    return actual_ms == test->expected_ms;
}

ER time_test_start(void)
{
    static const TimeTestCase tests[] = {
        /*
         * 40 ms warmup後の最初の有効PCM位置。
         */
        {
            .sample_position = 640ULL,
            .expected_ms = 40ULL
        },

        /*
         * 1秒。
         */
        {
            .sample_position = 16000ULL,
            .expected_ms = 1000ULL
        },

        /*
         * 1分。
         */
        {
            .sample_position = 960000ULL,
            .expected_ms = 60000ULL
        },

        /*
         * 10時間。
         */
        {
            .sample_position = 576000000ULL,
            .expected_ms = 36000000ULL
        }
    };

    unsigned passed = 0;

    tm_putstring(
        (UB *)
        "TIME_TEST: sample_position -> elapsed_ms\n");

    tm_printf(
        (UB *)
        "TIME_TEST: sample_rate=%u Hz\n",
        (unsigned)AUDIO_SAMPLE_RATE);

    for (unsigned i = 0;
         i < sizeof(tests) / sizeof(tests[0]);
         ++i) {

        if (time_test_run_case(
                i,
                &tests[i])) {
            ++passed;
        }
    }

    tm_printf(
        (UB *)
        "TIME_TEST: %u/%u passed.\n",
        passed,
        (unsigned)(
            sizeof(tests) /
            sizeof(tests[0])));

    if (passed !=
        sizeof(tests) /
        sizeof(tests[0])) {

        return E_IO;
    }

    tm_putstring(
        (UB *)
        "TIME_TEST: all passed.\n");

    return E_OK;
}