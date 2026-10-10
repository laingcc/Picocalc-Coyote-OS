#ifndef COYOTE_TIME_SYNC_H
#define COYOTE_TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wall-clock time, kept from a network time sync.
 *
 * time_sync_t holds the current UTC time as an epoch plus the monotonic clock,
 * and decides when the SNTP client should run and when the time is written to
 * storage.  It never touches the network or the card directly: the platform
 * supplies time_sync_ops_t, so the same code runs against lwIP SNTP in
 * firmware and a fake in the host tests.  This file and time_sync.c include
 * no lwIP or Pico SDK headers.
 *
 *   UNSET     no time is known
 *   RESTORED  the time saved before the last reboot, carried forward; it is
 *             behind by however long the unit was off
 *   SYNCED    set by the network since boot
 *
 * The SNTP client is started when the link comes up and stopped when it goes
 * down; while it runs it reports each answer through time_sync_on_sntp, and
 * its own timer re-syncs periodically.  Between answers the time advances with
 * the monotonic clock.
 *
 * The time is saved after every sync and then every
 * TIME_SYNC_SAVE_INTERVAL_MS, always from time_sync_poll and never from the
 * SNTP callback, so a reboot resumes at most that far behind the moment the
 * unit stopped.
 *
 * Times are seconds since 1970-01-01T00:00:00Z in a uint32_t (good until
 * 2106).  Everything kept and stored is UTC; only time_sync_label shifts the
 * time of day, by a fixed offset the caller supplies.  There are no named
 * time zones and no daylight saving rules.
 */

/* 2025-01-01T00:00:00Z.  Anything earlier, from the network or from storage,
 * is taken to be wrong and ignored. */
#define TIME_SYNC_EPOCH_MIN 1735689600u

#define TIME_SYNC_SAVE_INTERVAL_MS 600000u

/* "HH:MM" plus the trailing NUL. */
#define TIME_SYNC_HHMM_CAPACITY 6u
/* Longest time_sync_label ("HH:MM?") plus the trailing NUL. */
#define TIME_SYNC_LABEL_CAPACITY 7u
/* Longest stored record ("epoch=4294967295\n") plus the trailing NUL. */
#define TIME_SYNC_RECORD_CAPACITY 18u

typedef enum {
    TIME_SYNC_UNSET = 0,
    TIME_SYNC_RESTORED,
    TIME_SYNC_SYNCED
} time_sync_state_t;

/* Platform adapters.  All are required and are called with the context given
 * to time_sync_init. */
typedef struct {
    /* Monotonic milliseconds; may wrap. */
    uint32_t (*clock_ms)(void *context);
    /* Start the SNTP client without blocking; its answers arrive through
     * time_sync_on_sntp. */
    void (*sntp_start)(void *context);
    void (*sntp_stop)(void *context);
    /* Read the saved time.  Returns 0 and sets *epoch if there is one. */
    int (*load)(void *context, uint32_t *epoch);
    /* Write the time.  Returns 0 on success. */
    int (*save)(void *context, uint32_t epoch);
} time_sync_ops_t;

typedef struct {
    time_sync_state_t state;
    const time_sync_ops_t *ops;
    void *ops_context;
    bool sntp_running;
    bool save_pending; /* a sync has not been written yet */
    uint32_t epoch;    /* UTC seconds at base_ms */
    uint32_t base_ms;
    uint32_t saved_ms; /* when the last save was attempted */
} time_sync_t;

/* Reset the clock and adopt the saved time, if there is a plausible one. */
void time_sync_init(time_sync_t *sync, const time_sync_ops_t *ops, void *ops_context);

/*
 * Advance: start or stop the SNTP client as the link comes and goes, and save
 * the time when that is due.  Non-blocking apart from the save itself; must be
 * called more often than the monotonic clock wraps.
 */
void time_sync_poll(time_sync_t *sync, bool online);

/* An SNTP answer.  Safe from a network callback: it only records the time.
 * Answers before TIME_SYNC_EPOCH_MIN are ignored. */
void time_sync_on_sntp(time_sync_t *sync, uint32_t epoch);

time_sync_state_t time_sync_state(const time_sync_t *sync);

/* The current time.  Returns false, leaving *epoch alone, while UNSET. */
bool time_sync_now(const time_sync_t *sync, uint32_t *epoch);

/*
 * The clock as shown on a status line: "--:--" while UNSET, "HH:MM" once
 * SYNCED and "HH:MM?" while RESTORED.  The time of day is local: UTC plus
 * utc_offset_minutes, which may be negative.  Returns -1, writing "" if there
 * is room for it, when buffer cannot hold TIME_SYNC_LABEL_CAPACITY bytes.
 */
int time_sync_label(const time_sync_t *sync, int utc_offset_minutes, char *buffer, size_t capacity);

/* Write the UTC time of day of epoch as "HH:MM".  Returns -1, writing "" if
 * there is room for it, when buffer cannot hold TIME_SYNC_HHMM_CAPACITY
 * bytes. */
int time_sync_format_hhmm(uint32_t epoch, char *buffer, size_t capacity);

/*
 * The stored form of a time: the single line "epoch=<decimal seconds>\n".
 *
 * time_sync_record_format returns the length written, or -1 if buffer cannot
 * hold TIME_SYNC_RECORD_CAPACITY bytes.  time_sync_record_parse accepts that
 * line, with or without the line ending (LF or CRLF), and returns -1 for
 * anything else, including a value that does not fit or is earlier than
 * TIME_SYNC_EPOCH_MIN.
 */
int time_sync_record_format(uint32_t epoch, char *buffer, size_t capacity);
int time_sync_record_parse(const char *text, size_t length, uint32_t *epoch);

#endif
