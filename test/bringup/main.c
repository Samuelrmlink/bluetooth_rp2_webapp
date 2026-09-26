/*
 * Phase 0 bring-up. Proves the debug chain before any project code exists:
 * probe -> SWD -> flash -> run -> console, plus whether the target's USB cable
 * carries data at all (it did not enumerate when first plugged in, so this is
 * the diagnostic: if CDC appears, the cable is fine; if only RTT talks, it is not).
 *
 * Deliberately no cyw43/LED: the Pico 2 W's LED hangs off the radio chip, and
 * pulling that in would confuse "the debug chain works" with "the radio works".
 */
#include <stdio.h>
#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();

    for (uint32_t i = 0;; i++) {
        /* Both stdio backends are live, so this line goes out over RTT (SWD)
         * and over USB CDC. Whichever one we can read tells us what works. */
        printf("bringup: tick %lu\n", (unsigned long)i);
        sleep_ms(500);
    }
}
