#include "ai_preprocess.h"

#include <math.h>
#include <stddef.h>

#include "arm_math.h"
#include "stm32h5xx.h"

#include "hann_window.h"
#include "mel_filterbank.h"
#include "normalization_params.h"
#include "quantization_params.h"

/* -------------------------------------------------------------------------- */
/* Compile-time consistency checks                                            */
/* -------------------------------------------------------------------------- */

_Static_assert(
    AI_PREPROCESS_INPUT_SAMPLES == TRON_INPUT_SAMPLES,
    "Input sample count mismatch"
);

_Static_assert(
    AI_PREPROCESS_FRAME_LENGTH == TRON_FRAME_LENGTH,
    "Frame length mismatch"
);

_Static_assert(
    AI_PREPROCESS_HOP_LENGTH == TRON_FRAME_STEP,
    "Frame step mismatch"
);

_Static_assert(
    AI_PREPROCESS_FFT_LENGTH == TRON_FFT_LENGTH,
    "FFT length mismatch"
);

_Static_assert(
    AI_PREPROCESS_NUM_FRAMES == TRON_NUM_FRAMES,
    "Frame count mismatch"
);

_Static_assert(
    AI_PREPROCESS_NUM_FFT_BINS == TRON_NUM_FFT_BINS,
    "FFT bin count mismatch"
);

_Static_assert(
    AI_PREPROCESS_NUM_MEL_BINS == TRON_NUM_MEL_BINS,
    "Mel bin count mismatch"
);

_Static_assert(
    AI_PREPROCESS_OUTPUT_SIZE ==
        (AI_PREPROCESS_NUM_FRAMES * AI_PREPROCESS_NUM_MEL_BINS),
    "Unexpected AI preprocessing output size"
);

/* -------------------------------------------------------------------------- */
/* Internal work buffers                                                      */
/* -------------------------------------------------------------------------- */

/*
 * One STFT frame.
 *
 * [0 ... 399]
 *     int16 PCM -> float32 -> periodic Hann
 *
 * [400 ... 511]
 *     zero padding
 *
 * Reused for all 98 frames.
 */
static float s_fft_input[AI_PREPROCESS_FFT_LENGTH];

/*
 * CMSIS-DSP RFFT output.
 *
 * For a 512-point real FFT, CMSIS-DSP stores the output in packed form:
 *
 *   output[0] = Re(X[0])       : DC
 *   output[1] = Re(X[N/2])     : Nyquist
 *
 *   output[2*k]     = Re(X[k])
 *   output[2*k + 1] = Im(X[k])
 *
 * for k = 1 ... N/2 - 1.
 */
static float s_fft_output[AI_PREPROCESS_FFT_LENGTH];

/*
 * Power spectrum:
 *
 * bins 0 ... 256
 */
static float s_power_spectrum[AI_PREPROCESS_NUM_FFT_BINS];

/*
 * Mel-domain work buffer.
 *
 * Immediately after calculate_mel_power():
 *     Mel power
 *
 * After normalize_log_mel():
 *     normalized Log-Mel feature
 *
 * The same buffer is reused to reduce RAM usage.
 */
static float s_mel_power[AI_PREPROCESS_NUM_MEL_BINS];

/* CMSIS-DSP RFFT instance */
static arm_rfft_fast_instance_f32 s_rfft;

/* Initialization flag */
static bool s_rfft_initialized = false;

/* -------------------------------------------------------------------------- */
/* Sparse Mel-filter support                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Each triangular Mel filter is nonzero only over a limited range of
 * FFT bins.
 *
 * Instead of multiplying all 257 FFT bins by every one of the 40 Mel
 * filters, remember the first and last nonzero FFT bin for each filter.
 *
 * This preserves:
 *
 *   - the original filter coefficients
 *   - the original ascending accumulation order
 *   - the preprocessing specification
 *
 * Only multiplications by exact zero coefficients are omitted.
 */
static uint16_t s_mel_start_bin[
    AI_PREPROCESS_NUM_MEL_BINS
];

static uint16_t s_mel_end_bin[
    AI_PREPROCESS_NUM_MEL_BINS
];

static bool s_mel_support_initialized = false;

/* -------------------------------------------------------------------------- */
/* Internal functions                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the 512-point CMSIS-DSP real FFT.
 */
static bool init_fft(void)
{
    arm_status status;

    if (s_rfft_initialized) {
        return true;
    }

    status = arm_rfft_fast_init_f32(
        &s_rfft,
        AI_PREPROCESS_FFT_LENGTH
    );

    if (status != ARM_MATH_SUCCESS) {
        return false;
    }

    s_rfft_initialized = true;

    return true;
}


/**
 * @brief Determine the nonzero FFT-bin range of each Mel filter.
 *
 * This is performed only once.
 *
 * Example:
 *
 *   Mel filter m:
 *
 *       bins 0 ... 17   = 0
 *       bins 18 ... 27  = nonzero
 *       bins 28 ... 256 = 0
 *
 * then:
 *
 *       start_bin[m] = 18
 *       end_bin[m]   = 27
 *
 * Subsequent Mel calculations only evaluate bins 18 ... 27.
 */
static void init_mel_support(void)
{
    if (s_mel_support_initialized) {
        return;
    }

    for (uint32_t m = 0;
         m < AI_PREPROCESS_NUM_MEL_BINS;
         ++m) {

        uint32_t first =
            AI_PREPROCESS_NUM_FFT_BINS;

        uint32_t last = 0U;

        for (uint32_t k = 0;
             k < AI_PREPROCESS_NUM_FFT_BINS;
             ++k) {

            if (tron_mel_filterbank[k][m] != 0.0f) {

                if (first ==
                    AI_PREPROCESS_NUM_FFT_BINS) {

                    first = k;
                }

                last = k;
            }
        }

        /*
         * Normally every Mel filter has at least one nonzero
         * coefficient.
         *
         * Keep a safe representation even if a generated filter
         * unexpectedly contains no nonzero coefficients.
         */
        if (first ==
            AI_PREPROCESS_NUM_FFT_BINS) {

            s_mel_start_bin[m] = 0U;
            s_mel_end_bin[m] = 0U;
        }
        else {

            s_mel_start_bin[m] =
                (uint16_t)first;

            s_mel_end_bin[m] =
                (uint16_t)last;
        }
    }

    s_mel_support_initialized = true;
}


/**
 * @brief Enable the Cortex-M33 DWT cycle counter.
 */
static bool cycle_counter_enable(void)
{
    /*
     * Enable trace functionality.
     */
    CoreDebug->DEMCR |=
        CoreDebug_DEMCR_TRCENA_Msk;

    /*
     * Enable cycle counter.
     *
     * Do not reset CYCCNT here. Only cycle differences are used.
     */
    DWT->CTRL |=
        DWT_CTRL_CYCCNTENA_Msk;

    return
        (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}


/**
 * @brief Read the current DWT cycle counter.
 */
static inline uint32_t cycle_counter_get(void)
{
    return DWT->CYCCNT;
}


/**
 * @brief Prepare one STFT frame.
 *
 * Processing:
 *
 * int16 PCM
 *   -> float32 / 32768.0f
 *   -> periodic Hann window
 *   -> zero padding to 512 samples
 */
static void prepare_frame(
    const int16_t *pcm,
    uint32_t frame_index,
    float frame[AI_PREPROCESS_FFT_LENGTH]
)
{
    const uint32_t start =
        frame_index * AI_PREPROCESS_HOP_LENGTH;

    /*
     * First 400 samples:
     *
     * PCM -> float32 -> Hann
     */
    for (uint32_t i = 0;
         i < AI_PREPROCESS_FRAME_LENGTH;
         ++i) {

        const float sample =
            (float)pcm[start + i] / 32768.0f;

        frame[i] =
            sample * tron_hann_window[i];
    }

    /*
     * Zero padding:
     *
     * 400 -> 512 samples
     */
    for (uint32_t i = AI_PREPROCESS_FRAME_LENGTH;
         i < AI_PREPROCESS_FFT_LENGTH;
         ++i) {

        frame[i] = 0.0f;
    }
}


/**
 * @brief Calculate the 257-bin power spectrum from one 512-point frame.
 *
 * No additional FFT normalization is applied.
 */
static void calculate_power_spectrum(void)
{
    /*
     * Forward real FFT.
     *
     * ifftFlag = 0
     */
    arm_rfft_fast_f32(
        &s_rfft,
        s_fft_input,
        s_fft_output,
        0
    );

    /*
     * DC bin:
     *
     * X[0] is purely real.
     */
    s_power_spectrum[0] =
        s_fft_output[0] * s_fft_output[0];

    /*
     * Positive-frequency bins:
     *
     * k = 1 ... 255
     *
     * power[k] =
     *     Re(X[k])^2 + Im(X[k])^2
     */
    for (uint32_t k = 1;
         k < (AI_PREPROCESS_NUM_FFT_BINS - 1U);
         ++k) {

        const float real =
            s_fft_output[2U * k];

        const float imag =
            s_fft_output[2U * k + 1U];

        s_power_spectrum[k] =
            real * real +
            imag * imag;
    }

    /*
     * Nyquist bin:
     *
     * k = N/2 = 256
     *
     * CMSIS-DSP stores it at fft_output[1].
     */
    s_power_spectrum[
        AI_PREPROCESS_NUM_FFT_BINS - 1U
    ] =
        s_fft_output[1] *
        s_fft_output[1];
}


/**
 * @brief Apply the 40-bin Mel filterbank to the power spectrum.
 *
 * The generated filterbank is arranged as:
 *
 *   tron_mel_filterbank[fft_bin][mel_bin]
 *
 * Original calculation:
 *
 *   mel[m] =
 *       sum(k = 0 ... 256)
 *       (
 *           power[k] *
 *           tron_mel_filterbank[k][m]
 *       )
 *
 * The Mel filters are triangular and therefore sparse.
 *
 * The optimized implementation evaluates only the contiguous range
 * containing nonzero coefficients:
 *
 *   k = start_bin[m] ... end_bin[m]
 *
 * Multiplications by exact zero coefficients are omitted.
 */
static void calculate_mel_power(void)
{
    for (uint32_t m = 0;
         m < AI_PREPROCESS_NUM_MEL_BINS;
         ++m) {

        float sum = 0.0f;

        const uint32_t start =
            (uint32_t)s_mel_start_bin[m];

        const uint32_t end =
            (uint32_t)s_mel_end_bin[m];

        for (uint32_t k = start;
             k <= end;
             ++k) {

            sum +=
                s_power_spectrum[k] *
                tron_mel_filterbank[k][m];
        }

        s_mel_power[m] = sum;
    }
}


/**
 * @brief Convert Mel power to normalized Log-Mel features.
 *
 * Processing:
 *
 *   mel_power
 *       -> max(mel_power, TRON_LOG_FLOOR)
 *       -> 10 * log10(mel_power)
 *       -> (log_mel - mean[m]) / std[m]
 *
 * The normalized values overwrite s_mel_power[].
 */
static void normalize_log_mel(void)
{
    for (uint32_t m = 0;
         m < AI_PREPROCESS_NUM_MEL_BINS;
         ++m) {

        float mel_power =
            s_mel_power[m];

        /*
         * Avoid log10(0).
         */
        if (mel_power < TRON_LOG_FLOOR) {
            mel_power = TRON_LOG_FLOOR;
        }

        /*
         * Log-Mel:
         *
         * 10 * log10(Mel Power)
         */
        const float log_mel =
            10.0f * log10f(mel_power);

        /*
         * Protect normalization against an extremely
         * small standard deviation.
         */
        float std = tron_std[m];

        if (std < AI_PREPROCESS_STD_EPS) {
            std = AI_PREPROCESS_STD_EPS;
        }

        /*
         * TRAIN-only per-Mel-bin normalization.
         */
        s_mel_power[m] =
            (log_mel - tron_mean[m]) / std;
    }
}


/**
 * @brief Round float32 to the nearest integer using ties-to-even.
 *
 * Examples:
 *
 *   1.4  -> 1
 *   1.5  -> 2
 *   2.5  -> 2
 *  -1.5  -> -2
 *  -2.5  -> -2
 *
 * This intentionally does not use roundf(), because roundf()
 * rounds halfway cases away from zero.
 */
static int32_t round_ties_to_even_f32(float value)
{
    const float lower_f =
        floorf(value);

    const float fraction =
        value - lower_f;

    const int32_t lower =
        (int32_t)lower_f;

    if (fraction < 0.5f) {
        return lower;
    }

    if (fraction > 0.5f) {
        return lower + 1;
    }

    /*
     * Exactly halfway.
     *
     * Select the even integer.
     */
    if ((lower & 1) != 0) {
        return lower + 1;
    }

    return lower;
}


/**
 * @brief Quantize one normalized float32 feature to int8.
 *
 * q =
 *   clamp(
 *       ties_to_even(value / TRON_INPUT_SCALE)
 *       + TRON_INPUT_ZERO_POINT,
 *       -128,
 *       127
 *   )
 */
static int8_t quantize_input_value(float value)
{
    const float scaled =
        value / TRON_INPUT_SCALE;

    int32_t quantized =
        round_ties_to_even_f32(scaled);

    quantized +=
        (int32_t)TRON_INPUT_ZERO_POINT;

    if (quantized < -128) {
        quantized = -128;
    }
    else if (quantized > 127) {
        quantized = 127;
    }

    return (int8_t)quantized;
}


/**
 * @brief Shared preprocessing implementation.
 *
 * When profile is NULL, no stage-by-stage cycle measurement is performed.
 *
 * When profile is non-NULL, DWT cycle counts are accumulated for each stage.
 */
static bool preprocess_internal(
    const int16_t pcm[AI_PREPROCESS_INPUT_SAMPLES],
    int8_t output[AI_PREPROCESS_OUTPUT_SIZE],
    ai_preprocess_profile_t *profile
)
{
    uint32_t total_start = 0U;

    if ((pcm == NULL) || (output == NULL)) {
        return false;
    }

    /*
     * Initialize CMSIS-DSP RFFT once.
     *
     * Initialization is intentionally outside the profiled total interval.
     */
    if (!init_fft()) {
        return false;
    }

    /*
     * Determine the sparse support of every Mel filter once.
     *
     * This depends only on the generated filterbank and therefore does not
     * need to be repeated for every audio event.
     *
     * Initialization is intentionally outside the profiled total interval.
     */
    init_mel_support();

    if (profile != NULL) {

        if (!cycle_counter_enable()) {
            return false;
        }

        profile->frame_cycles = 0ULL;
        profile->fft_power_cycles = 0ULL;
        profile->mel_cycles = 0ULL;
        profile->log_norm_quant_cycles = 0ULL;
        profile->total_cycles = 0ULL;

        total_start =
            cycle_counter_get();
    }

    /*
     * Process all 98 STFT frames.
     *
     * center = false
     * no signal-boundary padding
     *
     * frame start:
     *
     *   frame_index * 160
     */
    for (uint32_t frame_index = 0;
         frame_index < AI_PREPROCESS_NUM_FRAMES;
         ++frame_index) {

        /*
         * --------------------------------------------------------------
         * Stage 1:
         *
         * PCM
         * -> float32
         * -> Hann
         * -> zero-padded 512 samples
         * --------------------------------------------------------------
         */

        if (profile != NULL) {

            const uint32_t start =
                cycle_counter_get();

            prepare_frame(
                pcm,
                frame_index,
                s_fft_input
            );

            const uint32_t end =
                cycle_counter_get();

            profile->frame_cycles +=
                (uint32_t)(end - start);
        }
        else {

            prepare_frame(
                pcm,
                frame_index,
                s_fft_input
            );
        }

        /*
         * --------------------------------------------------------------
         * Stage 2:
         *
         * 512-point RFFT
         * -> 257-bin power spectrum
         * --------------------------------------------------------------
         */

        if (profile != NULL) {

            const uint32_t start =
                cycle_counter_get();

            calculate_power_spectrum();

            const uint32_t end =
                cycle_counter_get();

            profile->fft_power_cycles +=
                (uint32_t)(end - start);
        }
        else {

            calculate_power_spectrum();
        }

        /*
         * --------------------------------------------------------------
         * Stage 3:
         *
         * 257-bin power spectrum
         * -> 40-bin Mel power
         * --------------------------------------------------------------
         */

        if (profile != NULL) {

            const uint32_t start =
                cycle_counter_get();

            calculate_mel_power();

            const uint32_t end =
                cycle_counter_get();

            profile->mel_cycles +=
                (uint32_t)(end - start);
        }
        else {

            calculate_mel_power();
        }

        /*
         * --------------------------------------------------------------
         * Stage 4:
         *
         * 40-bin Mel power
         * -> Log-Mel
         * -> normalization
         * -> int8 quantization
         * --------------------------------------------------------------
         */

        if (profile != NULL) {

            const uint32_t start =
                cycle_counter_get();

            normalize_log_mel();

            for (uint32_t mel_bin = 0;
                 mel_bin < AI_PREPROCESS_NUM_MEL_BINS;
                 ++mel_bin) {

                const uint32_t output_index =
                    frame_index *
                        AI_PREPROCESS_NUM_MEL_BINS +
                    mel_bin;

                output[output_index] =
                    quantize_input_value(
                        s_mel_power[mel_bin]
                    );
            }

            const uint32_t end =
                cycle_counter_get();

            profile->log_norm_quant_cycles +=
                (uint32_t)(end - start);
        }
        else {

            normalize_log_mel();

            for (uint32_t mel_bin = 0;
                 mel_bin < AI_PREPROCESS_NUM_MEL_BINS;
                 ++mel_bin) {

                const uint32_t output_index =
                    frame_index *
                        AI_PREPROCESS_NUM_MEL_BINS +
                    mel_bin;

                output[output_index] =
                    quantize_input_value(
                        s_mel_power[mel_bin]
                    );
            }
        }
    }

    if (profile != NULL) {

        const uint32_t total_end =
            cycle_counter_get();

        profile->total_cycles =
            (uint32_t)(
                total_end -
                total_start
            );
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bool ai_preprocess_run(
    const int16_t pcm[AI_PREPROCESS_INPUT_SAMPLES],
    int8_t output[AI_PREPROCESS_OUTPUT_SIZE]
)
{
    return preprocess_internal(
        pcm,
        output,
        NULL
    );
}


bool ai_preprocess_run_profiled(
    const int16_t pcm[AI_PREPROCESS_INPUT_SAMPLES],
    int8_t output[AI_PREPROCESS_OUTPUT_SIZE],
    ai_preprocess_profile_t *profile
)
{
    if (profile == NULL) {
        return false;
    }

    return preprocess_internal(
        pcm,
        output,
        profile
    );
}