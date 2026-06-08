/* Host-test stub for pico-sdk pico/time.h.
 * The test controls "time" via host_test_set_now_ms() so staleness logic and
 * read timeouts are deterministic. */
#ifndef HOST_STUB_PICO_TIME_H
#define HOST_STUB_PICO_TIME_H
#include <stdint.h>
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t t);
uint64_t time_us_64(void);
#endif
