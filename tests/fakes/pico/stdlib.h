#ifndef COYOTE_TEST_FAKE_PICO_STDLIB_H
#define COYOTE_TEST_FAKE_PICO_STDLIB_H

/* Host stand-in for pico/stdlib.h: time stands still and sleeping returns
 * at once. */

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) { return 0u; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000u); }
static inline void sleep_ms(uint32_t ms) { (void)ms; }

#endif
