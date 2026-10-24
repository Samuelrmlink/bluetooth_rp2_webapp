/* Who called panic, kept where a reverted trial can still read it.
 *
 * 1.249 and 1.250 both panicked before the first main-loop crumb. The fault
 * frame names panic's own breakpoint, and the caller's return address sat
 * above that frame, where the next boot's stack paint wipes it. scratch[0]
 * is what the next image reports as last_crumb. Nothing has written it yet
 * on that path (crumb_boot clears it; crumb() is the only other writer, and
 * it runs from the loop). scratch[1] stays the fault PC. Scratch 2 and 3
 * belong to the ROM.
 *
 * --wrap catches a direct bl. A tail branch names the caller's caller.
 * The stock panic prints fmt from its own frame, so only fmt is passed on.
 */
#include "hardware/structs/watchdog.h"

#include <stdint.h>

void __attribute__((noreturn)) __real_panic(const char *fmt, ...);

void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    watchdog_hw->scratch[0] = (uint32_t)__builtin_return_address(0);
    __real_panic(fmt);
}
