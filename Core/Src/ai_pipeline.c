#include "ai_pipeline.h"

#include <stddef.h>

#include "network.h"

/* -------------------------------------------------------------------------- */
/* Internal buffers                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Shared preprocessing -> inference buffer.
 *
 * Data flow:
 *
 *   PCM[16000]
 *       ↓
 *   ai_preprocess_run()
 *       ↓
 *   s_model_input[3920]
 *       ↓
 *   ai_classifier_predict()
 *
 * The classifier binds this buffer directly to the ST Edge AI input tensor.
 * No second 3920-byte classifier-side input buffer is required.
 */
STAI_ALIGNED(32)
static int8_t s_model_input[AI_INPUT_SIZE];

/* -------------------------------------------------------------------------- */
/* Internal state                                                             */
/* -------------------------------------------------------------------------- */

static bool s_initialized = false;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bool ai_pipeline_init(void)
{
    if (s_initialized) {
        return true;
    }

    if (!ai_classifier_init()) {
        return false;
    }

    s_initialized = true;

    return true;
}


bool ai_pipeline_predict(
    const int16_t pcm[AI_PIPELINE_INPUT_SAMPLES],
    ai_pipeline_result_t *result
)
{
    ai_classifier_result_t classifier_result;

    if ((pcm == NULL) || (result == NULL)) {
        return false;
    }

    if (!s_initialized) {
        return false;
    }

    /*
     * PCM[16000]
     *     ↓
     * D-07 preprocessing
     *     ↓
     * aligned int8 model input[3920]
     */
    if (!ai_preprocess_run(
            pcm,
            s_model_input
        )) {

        return false;
    }

    /*
     * Run the quantized CNN.
     *
     * ai_classifier_predict() binds s_model_input directly to the
     * generated ST Edge AI input tensor.
     */
    if (!ai_classifier_predict(
            s_model_input,
            &classifier_result
        )) {

        return false;
    }

    result->class_id =
        classifier_result.class_id;

    for (uint32_t i = 0U;
         i < AI_PIPELINE_NUM_CLASSES;
         ++i) {

        result->raw_output[i] =
            classifier_result.raw_output[i];
    }

    /*
     * Output quantization:
     *
     *   real = (q - (-128)) / 256
     *        = (q + 128) / 256
     *
     * Preserve the numerator as Q8 confidence.
     */
    result->confidence_q8 =
        (uint8_t)(
            (int16_t)classifier_result.raw_output[
                (uint32_t)classifier_result.class_id
            ] +
            128
        );

    return true;
}