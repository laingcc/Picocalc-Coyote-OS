#ifndef COYOTE_TIME_SERVICE_H
#define COYOTE_TIME_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "time/time_sync.h"

/*
 * Firmware binding for the clock.
 *
 * time_service owns the single static time_sync_t and wires it to the lwIP
 * SNTP client and to <dir>/clock.txt on the SD card.  The SNTP client runs
 * while app_services reports Wi-Fi online: it asks TIME_SERVICE_NTP_SERVER as
 * soon as the link is up and again every hour, entirely from lwIP timers and
 * callbacks inside app_services_poll.  Nothing here blocks on the network or
 * allocates.
 *
 * When the board has no CYW43 radio the same interface is built without SNTP:
 * the clock only ever carries the saved time forward.
 */

#define TIME_SERVICE_NTP_SERVER "pool.ntp.org"
#define TIME_SERVICE_FILE_NAME "clock.txt"

/* Longest accepted "<dir>/clock.txt" path, including the trailing NUL. */
#define TIME_SERVICE_PATH_CAPACITY 64u

/* Reset the clock and restore the saved time from dir.  Call once at startup,
 * after the filesystem is mounted and after app_services_init. */
void time_service_init(const char *dir);

/* Follow the Wi-Fi state and save the time when due.  Cheap; call it next to
 * app_services_poll. */
void time_service_poll(void);

time_sync_state_t time_service_state(void);

/* The current UTC time in seconds since 1970.  Returns false, leaving *epoch
 * alone, if no time is known yet. */
bool time_service_now(uint32_t *epoch);

/* The clock for a status line, in local time; see time_sync_label. */
int time_service_label(int utc_offset_minutes, char *buffer, size_t capacity);

/* Called by the lwIP SNTP client (SNTP_SET_SYSTEM_TIME in lwipopts.h). */
void time_service_on_sntp(uint32_t epoch);

#endif
