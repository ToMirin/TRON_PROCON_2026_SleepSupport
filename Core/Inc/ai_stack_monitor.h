#ifndef AI_STACK_MONITOR_H
#define AI_STACK_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t configured_bytes;

    /*
     * Number of bytes from _estack down to the lowest address whose
     * watermark has been overwritten.
     *
     * When watermark_touched == false, this is an upper bound determined
     * by the initialization guard region.
     */
    uint32_t used_bytes;

    uint32_t remaining_bytes;

    /*
     * Number of bytes actually initialized with the watermark pattern.
     */
    uint32_t watermarked_bytes;

    /*
     * true:
     *   Execution overwrote at least part of the initialized watermark area.
     *
     * false:
     *   Execution never reached the initialized watermark area.
     *   In this case used_bytes is an upper bound, not an exact high-water mark.
     */
    bool watermark_touched;

} ai_stack_monitor_result_t;


/*
 * Initialize the currently unused part of the MSP stack with a watermark.
 *
 * Must be called before the code region whose maximum stack usage is
 * to be measured.
 */
bool ai_stack_monitor_init(void);


/*
 * Scan the watermark and obtain the stack high-water mark.
 */
bool ai_stack_monitor_get_result(
    ai_stack_monitor_result_t *result
);


#ifdef __cplusplus
}
#endif

#endif /* AI_STACK_MONITOR_H */