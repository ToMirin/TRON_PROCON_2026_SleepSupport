#ifndef AI_CLASSIFIER_H
#define AI_CLASSIFIER_H

#include <stdint.h>
#include <stdbool.h>

#include "quantization_params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_INPUT_FRAMES      (98)
#define AI_INPUT_MEL_BINS    (40)
#define AI_INPUT_CHANNELS    (1)

#define AI_INPUT_SIZE \
    (AI_INPUT_FRAMES * AI_INPUT_MEL_BINS * AI_INPUT_CHANNELS)

#define AI_OUTPUT_SIZE       (3)

typedef enum
{
    AI_CLASS_OTHER = TRON_CLASS_OTHER,
    AI_CLASS_SNORE = TRON_CLASS_SNORE,
    AI_CLASS_COUGH = TRON_CLASS_COUGH
} ai_class_t;

typedef struct
{
    ai_class_t class_id;
    int8_t raw_output[AI_OUTPUT_SIZE];
} ai_classifier_result_t;


/**
 * @brief Initialize the generated ST Edge AI classifier.
 *
 * Activation and output buffers are configured here.
 *
 * The input tensor buffer is supplied by the caller for each inference,
 * allowing the preprocessing output buffer to be used directly without
 * an additional 3920-byte copy.
 *
 * @return true on success, false on initialization failure.
 */
bool ai_classifier_init(void);


/**
 * @brief Run one synchronous inference.
 *
 * The caller-provided input buffer is bound directly to the generated
 * network input tensor. The function does not copy the 3920-byte input.
 *
 * The input buffer must remain valid and unchanged until this function
 * returns.
 *
 * @param input
 *        Quantized model input tensor.
 *        Exactly 3920 signed int8 elements.
 *
 * @param output
 *        Destination for the three signed-int8 model outputs.
 *
 * @return true on success, false on invalid arguments or inference failure.
 */
bool ai_classifier_run(
    const int8_t input[AI_INPUT_SIZE],
    int8_t output[AI_OUTPUT_SIZE]
);


ai_class_t ai_classifier_argmax(
    const int8_t output[AI_OUTPUT_SIZE]
);


bool ai_classifier_predict(
    const int8_t input[AI_INPUT_SIZE],
    ai_classifier_result_t *result
);

#ifdef __cplusplus
}
#endif

#endif /* AI_CLASSIFIER_H */