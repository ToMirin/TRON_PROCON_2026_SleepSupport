#ifndef AI_PIPELINE_H
#define AI_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "ai_classifier.h"
#include "ai_preprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public constants                                                           */
/* -------------------------------------------------------------------------- */

#define AI_PIPELINE_INPUT_SAMPLES \
    AI_PREPROCESS_INPUT_SAMPLES

#define AI_PIPELINE_NUM_CLASSES \
    AI_OUTPUT_SIZE

/* -------------------------------------------------------------------------- */
/* Compile-time consistency checks                                            */
/* -------------------------------------------------------------------------- */

_Static_assert(
    AI_PREPROCESS_OUTPUT_SIZE == AI_INPUT_SIZE,
    "Preprocessing output size and classifier input size mismatch"
);

/* -------------------------------------------------------------------------- */
/* Result                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct
{
    /*
     * Predicted class.
     *
     * OTHER = 0
     * SNORE = 1
     * COUGH = 2
     */
    ai_class_t class_id;

    /*
     * Raw signed-int8 model outputs.
     */
    int8_t raw_output[AI_PIPELINE_NUM_CLASSES];

    /*
     * Quantized confidence of the predicted class.
     *
     * Model output quantization:
     *
     *   scale      = 1 / 256
     *   zero-point = -128
     *
     * Therefore:
     *
     *   confidence_q8 =
     *       raw_output[class_id] + 128
     *
     * Range:
     *
     *   0 ... 255
     *
     * Approximate floating-point value:
     *
     *   confidence ~= confidence_q8 / 256
     */
    uint8_t confidence_q8;

} ai_pipeline_result_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the AI inference pipeline.
 *
 * This function initializes the classifier.
 *
 * Preprocessing-specific state such as the CMSIS-DSP RFFT instance and
 * sparse Mel-filter support is initialized lazily on the first call to
 * ai_pipeline_predict().
 *
 * This function may be called more than once.
 *
 * @return true on success, false on initialization failure.
 */
bool ai_pipeline_init(void);


/**
 * @brief Run preprocessing and inference for one 1-second PCM window.
 *
 * Input contract:
 *
 * - sample rate: 16 kHz
 * - channels: mono
 * - format: signed int16 PCM
 * - length: exactly 16000 samples
 *
 * The PCM buffer is borrowed and read-only.
 *
 * The pipeline does not copy or retain the PCM buffer after this function
 * returns.
 *
 * The caller must not modify the PCM buffer while this function is running.
 *
 * This module is blocking and non-reentrant.
 *
 * Do not call this function:
 *
 * - from an ISR
 * - concurrently from multiple tasks
 * - again before the previous call has completed
 *
 * If fewer than 16000 samples are available, the caller must not invoke this
 * function. The pipeline does not perform zero-padding for an incomplete
 * event window.
 *
 * @param pcm
 *        Exactly 16000 signed 16-bit PCM samples.
 *
 * @param result
 *        Destination for class, raw outputs and quantized confidence.
 *
 * @return true on success.
 *         false on invalid arguments, preprocessing failure or inference
 *         failure.
 */
bool ai_pipeline_predict(
    const int16_t pcm[AI_PIPELINE_INPUT_SAMPLES],
    ai_pipeline_result_t *result
);

#ifdef __cplusplus
}
#endif

#endif /* AI_PIPELINE_H */