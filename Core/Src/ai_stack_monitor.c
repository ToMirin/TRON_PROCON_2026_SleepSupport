#include "ai_stack_monitor.h"

#include "main.h"

#include <stddef.h>
#include <stdint.h>


/*
 * Linker symbols defined in STM32H533RETX_FLASH.ld / RAM.ld.
 *
 * IMPORTANT:
 * These symbols represent addresses.
 * Therefore their addresses (&_sstack / &_estack) must be used.
 */
extern uint8_t _sstack;
extern uint8_t _estack;


/*
 * 32-bit watermark pattern.
 */
#define AI_STACK_WATERMARK_PATTERN    (0xA5A5A5A5UL)

/*
 * Do not write immediately below the current MSP.
 *
 * This protects the stack currently being used by
 * ai_stack_monitor_init() itself and leaves additional safety margin.
 */
#define AI_STACK_INIT_GUARD_BYTES     (256U)


static uintptr_t s_stack_bottom = 0U;
static uintptr_t s_stack_top = 0U;
static uintptr_t s_watermark_end = 0U;

static bool s_initialized = false;


/*
 * Align address downward to a 32-bit boundary.
 */
static uintptr_t align_down_4(
    uintptr_t address
)
{
    return address & ~(uintptr_t)0x3U;
}


bool ai_stack_monitor_init(void)
{
    uintptr_t stack_bottom;
    uintptr_t stack_top;
    uintptr_t current_msp;
    uintptr_t watermark_end;

    uint32_t primask;
    volatile uint32_t *p;
    volatile uint32_t *end;


    stack_bottom =
        (uintptr_t)&_sstack;

    stack_top =
        (uintptr_t)&_estack;

    /*
     * Sanity check linker symbols.
     */
    if (stack_top <= stack_bottom)
    {
        return false;
    }

    /*
     * Read MSP while executing in Thread mode before μT-Kernel starts.
     */
    current_msp =
        (uintptr_t)__get_MSP();

    /*
     * MSP must currently lie inside the configured stack region.
     */
    if ((current_msp <= stack_bottom) ||
        (current_msp > stack_top))
    {
        return false;
    }

    /*
     * Ensure there is enough unused stack below the current MSP
     * to create a meaningful watermark region.
     */
    if (current_msp <=
        (stack_bottom + AI_STACK_INIT_GUARD_BYTES))
    {
        return false;
    }

    watermark_end =
        current_msp - AI_STACK_INIT_GUARD_BYTES;

    watermark_end =
        align_down_4(watermark_end);

    if (watermark_end <= stack_bottom)
    {
        return false;
    }


    /*
     * Disable interrupts while filling the currently unused stack.
     *
     * An interrupt entered during this operation would also use MSP.
     * Preventing interrupts here avoids a race with the area being filled.
     */
    primask =
        __get_PRIMASK();

    __disable_irq();

    p =
        (volatile uint32_t *)stack_bottom;

    end =
        (volatile uint32_t *)watermark_end;

    while (p < end)
    {
        *p =
            AI_STACK_WATERMARK_PATTERN;

        ++p;
    }


    /*
     * Ensure watermark writes are globally visible before restoring IRQs.
     */
    __DSB();
    __ISB();


    if (primask == 0U)
    {
        __enable_irq();
    }


    s_stack_bottom =
        stack_bottom;

    s_stack_top =
        stack_top;

    s_watermark_end =
        watermark_end;

    s_initialized =
        true;


    return true;
}


bool ai_stack_monitor_get_result(
    ai_stack_monitor_result_t *result
)
{
    volatile const uint32_t *p;
    volatile const uint32_t *end;

    uintptr_t first_changed;
    uintptr_t lowest_checked;

    uint32_t configured_bytes;
    uint32_t watermarked_bytes;
    uint32_t used_bytes;

    bool watermark_touched;


    if ((result == NULL) ||
        (!s_initialized))
    {
        return false;
    }


    p =
        (volatile const uint32_t *)s_stack_bottom;

    end =
        (volatile const uint32_t *)s_watermark_end;


    /*
     * Stack grows from high addresses toward low addresses.
     *
     * Scan upward from _sstack until the first modified watermark word.
     */
    while ((p < end) &&
           (*p == AI_STACK_WATERMARK_PATTERN))
    {
        ++p;
    }


    configured_bytes =
        (uint32_t)(
            s_stack_top -
            s_stack_bottom
        );

    watermarked_bytes =
        (uint32_t)(
            s_watermark_end -
            s_stack_bottom
        );


    if (p < end)
    {
        /*
         * The benchmark reached the watermarked region.
         *
         * p is the lowest-address watermark word that was overwritten.
         * Everything from here to _estack belongs to the observed
         * high-water stack range.
         */
        first_changed =
            (uintptr_t)p;

        used_bytes =
            (uint32_t)(
                s_stack_top -
                first_changed
            );

        watermark_touched =
            true;
    }
    else
    {
        /*
         * No initialized watermark word was modified.
         *
         * Because the region between s_watermark_end and the MSP observed
         * during initialization was intentionally left untouched, the exact
         * high-water mark cannot be determined.
         *
         * Report the conservative upper bound.
         */
        lowest_checked =
            s_watermark_end;

        used_bytes =
            (uint32_t)(
                s_stack_top -
                lowest_checked
            );

        watermark_touched =
            false;
    }


    if (used_bytes > configured_bytes)
    {
        /*
         * This should never happen unless the linker configuration or
         * stack state is inconsistent.
         */
        return false;
    }


    result->configured_bytes =
        configured_bytes;

    result->used_bytes =
        used_bytes;

    result->remaining_bytes =
        configured_bytes - used_bytes;

    result->watermarked_bytes =
        watermarked_bytes;

    result->watermark_touched =
        watermark_touched;


    return true;
}