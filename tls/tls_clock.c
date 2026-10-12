#include <stdint.h>
#include <time.h>

#include "pico/time.h"

#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"

#include "time/time_service.h"

/*
 * The clocks mbedTLS is built to use (see tls/mbedtls_config.h).
 *
 * coyote_tls_time is the only source of wall time for certificate checks.  It
 * reports the time only once SNTP has set it since boot; a time restored from
 * the SD card is stale by however long the unit was off and is not offered.
 * Until then it reports 0 (1970), at which every certificate is not yet
 * valid, so a handshake attempted without a real clock fails verification
 * instead of skipping the date check.
 */
time_t coyote_tls_time(time_t *now) {
    uint32_t epoch = 0u;
    time_t value = time_service_trusted_now(&epoch) ? (time_t)epoch : (time_t)0;

    if (now != NULL) {
        *now = value;
    }
    return value;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf) {
    time_sync_civil_t civil;

    if (tt == NULL || tm_buf == NULL || *tt < 0 || (uint64_t)*tt > UINT32_MAX) {
        return NULL;
    }
    time_sync_civil((uint32_t)*tt, &civil);
    tm_buf->tm_year = civil.year - 1900;
    tm_buf->tm_mon = civil.month - 1;
    tm_buf->tm_mday = civil.day;
    tm_buf->tm_hour = civil.hour;
    tm_buf->tm_min = civil.minute;
    tm_buf->tm_sec = civil.second;
    tm_buf->tm_wday = 0;
    tm_buf->tm_yday = 0;
    tm_buf->tm_isdst = 0;
    return tm_buf;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(to_us_since_boot(get_absolute_time()) / 1000u);
}
