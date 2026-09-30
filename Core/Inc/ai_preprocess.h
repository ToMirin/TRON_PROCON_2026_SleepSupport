#ifndef AI_PREPROCESS_H
#define AI_PREPROCESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Input specification                                                        */
/* -------------------------------------------------------------------------- */

/* 16 kHz, mono, 1.0 s */
#define AI_PREPROCESS_SAMPLE_RATE      (16000)
#define AI_PREPROCESS_INPUT_SAMPLES    (16000)

/* -------------------------------------------------------------------------- */
/* STFT specification                                                         */
/* -------------------------------------------------------------------------- */

/* 25 ms frame @ 16 kHz */
#define AI_PREPROCESS_FRAME_LENGTH     (400)

/* 10 ms hop @ 16 kHz */
#define AI_PREPROCESS_HOP_LENGTH       (160)

#define AI_PREPROCESS_FFT_LENGTH       (512)

/*
 * center = false, no padding
 *
 * floor((16000 - 400) / 160) + 1 = 98
 */
#define AI_PREPROCESS_NUM_FRAMES       (98)

/* Real FFT: 0 ... Nyquist */
#define AI_PREPROCESS_NUM_FFT_BINS     (257)

/* -------------------------------------------------------------------------- */
/* Mel specification                                                          */
/* -------------------------------------------------------------------------- */

#define AI_PREPROCESS_NUM_MEL_BINS     (40)

#define AI_PREPROCESS_FMIN_HZ          (60.0f)
#define AI_PREPROCESS_FMAX_HZ          (8000.0f)

#define AI_PREPROCESS_LOG_FLOOR        (1.0e-10f)
#define AI_PREPROCESS_STD_EPS          (1.0e-8f)

/* -------------------------------------------------------------------------- */
/* Output specification                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Logical model input:
 *
 *   [1, 98, 40, 1]
 *
 * Flattened layout:
 *
 *   output[time_frame * 40 + mel_bin]
 */
#define AI_PREPROCESS_OUTPUT_SIZE \
    (AI_PREPROCESS_NUM_FRAMES * AI_PREPROCESS_NUM_MEL_BINS)

/* -------------------------------------------------------------------------- */
/* Profiling                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Cycle counts for each preprocessing stage.
 *
 * These values are intended for performance analysis only.
 *
 * The normal ai_preprocess_run() path does not perform stage-by-stage
 * profiling.
 */
typedef struct
{
    /*
     * PCM int16
     *   -> float32
     *   -> Hann window
     *   -> zero padding
     */
    uint64_t frame_cycles;

    /*
     * 512-point RFFT
     *   -> 257-bin power spectrum
     */
    uint64_t fft_power_cycles;

    /*
     * 257-bin power spectrum
     *   -> 40-bin Mel power
     */
    uint64_t mel_cycles;

    /*
     * Mel power
     *   -> log10
     *   -> normalization
     *   -> int8 quantization
     */
    uint64_t log_norm_quant_cycles;

    /*
     * Entire processing loop for all 98 frames.
     *
     * FFT initialization is excluded.
     */
    uint64_t total_cycles;

} ai_preprocess_profile_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Convert 1 second PCM audio into the quantized CNN input tensor.
 *
 * Processing:
 *
 * int16 PCM
 *   -> float / 32768.0f
 *   -> periodic Hann
 *   -> 512-point STFT
 *   -> power spectrum
 *   -> HTK Mel filterbank
 *   -> 10*log10(max(power, 1e-10))
 *   -> per-Mel-bin normalization
 *   -> int8 quantization
 *
 * @param pcm
 *        Input mono PCM buffer.
 *        Exactly 16000 signed 16-bit samples.
 *
 * @param output
 *        Output int8 tensor.
 *        Exactly 98 * 40 = 3920 elements.
 *
 * @return true on success, false on invalid arguments or processing failure.
 */
bool ai_preprocess_run(
    const int16_t pcm[AI_PREPROCESS_INPUT_SAMPLES],
    int8_t output[AI_PREPROCESS_OUTPUT_SIZE]
);


/**
 * @brief Run preprocessing while collecting stage-by-stage cycle counts.
 *
 * The generated output tensor is identical to ai_preprocess_run().
 *
 * Profiling uses the Cortex-M33 DWT CYCCNT cycle counter.
 *
 * FFT initialization is performed before the total timing interval so that
 * the result represents steady-state per-event preprocessing time.
 *
 * @param pcm
 *        Input mono PCM buffer.
 *
 * @param output
 *        Quantized model input tensor.
 *
 * @param profile
 *        Destination for cycle-count measurements.
 *
 * @return true on success, false on invalid arguments or processing failure.
 */
bool ai_preprocess_run_profiled(
    const int16_t pcm[AI_PREPROCESS_INPUT_SAMPLES],
    int8_t output[AI_PREPROCESS_OUTPUT_SIZE],
    ai_preprocess_profile_t *profile
);

#ifdef __cplusplus
}
#endif

#endif /* AI_PREPROCESS_H */