#include "ai/time_service.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "ai/app_services.h"

#ifndef COYOTE_HAS_WIFI
#define COYOTE_HAS_WIFI 0
#endif

#if COYOTE_HAS_WIFI
#include "lwip/apps/sntp.h"
#endif

static struct {
    bool initialised;
    time_sync_t sync;
    char path[TIME_SERVICE_PATH_CAPACITY];
} service;

static uint32_t platform_clock_ms(void *context) {
    (void)context;
    return to_ms_since_boot(get_absolute_time());
}

#if COYOTE_HAS_WIFI

/*
 * The link is only ever reported up after app_services has brought up CYW43,
 * and with it lwIP, so the SNTP client is never started before lwip_init.
 * With pico_cyw43_arch_lwip_poll its timers and callbacks run inside
 * cyw43_arch_poll on the main loop, the same context as these calls, so no
 * locking is needed.
 */
static void platform_sntp_start(void *context) {
    (void)context;
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, TIME_SERVICE_NTP_SERVER);
    sntp_init();
}

static void platform_sntp_stop(void *context) {
    (void)context;
    sntp_stop();
}

#else /* !COYOTE_HAS_WIFI */

static void platform_sntp_start(void *context) {
    (void)context;
}

static void platform_sntp_stop(void *context) {
    (void)context;
}

#endif /* COYOTE_HAS_WIFI */

static int platform_load(void *context, uint32_t *epoch) {
    char record[TIME_SYNC_RECORD_CAPACITY];
    size_t length;
    FILE *file;
    (void)context;

    if (service.path[0] == '\0') {
        return -1;
    }
    file = fopen(service.path, "rb");
    if (file == NULL) {
        return -1;
    }
    length = fread(record, 1u, sizeof(record), file);
    fclose(file);
    return time_sync_record_parse(record, length, epoch);
}

/* Written in place: a save cut short by power loss leaves a record that does
 * not parse, and the clock then simply starts unset. */
static int platform_save(void *context, uint32_t epoch) {
    char record[TIME_SYNC_RECORD_CAPACITY];
    int length = time_sync_record_format(epoch, record, sizeof(record));
    int failed;
    FILE *file;
    (void)context;

    if (length < 0 || service.path[0] == '\0') {
        return -1;
    }
    file = fopen(service.path, "wb");
    if (file == NULL) {
        return -1;
    }
    failed = fwrite(record, 1u, (size_t)length, file) != (size_t)length;
    failed |= fclose(file) != 0;
    return failed ? -1 : 0;
}

static const time_sync_ops_t sync_ops = {
    platform_clock_ms, platform_sntp_start, platform_sntp_stop, platform_load, platform_save,
};

void time_service_init(const char *dir) {
    int length;

    memset(&service, 0, sizeof(service));
    length = dir != NULL ? snprintf(service.path, sizeof(service.path), "%s/%s", dir, TIME_SERVICE_FILE_NAME) : -1;
    if (length < 0 || (size_t)length >= sizeof(service.path)) {
        /* No usable path: the clock still runs, it is just never stored. */
        service.path[0] = '\0';
    }
    time_sync_init(&service.sync, &sync_ops, NULL);
    service.initialised = true;
}

void time_service_poll(void) {
    if (!service.initialised) {
        return;
    }
    time_sync_poll(&service.sync, app_services_wifi_state() == WIFI_STATE_ONLINE);
}

time_sync_state_t time_service_state(void) {
    return service.initialised ? time_sync_state(&service.sync) : TIME_SYNC_UNSET;
}

bool time_service_now(uint32_t *epoch) {
    return service.initialised && time_sync_now(&service.sync, epoch);
}

int time_service_label(char *buffer, size_t capacity) {
    if (!service.initialised) {
        if (buffer != NULL && capacity > 0u) {
            buffer[0] = '\0';
        }
        return -1;
    }
    return time_sync_label(&service.sync, buffer, capacity);
}

void time_service_on_sntp(uint32_t epoch) {
    if (!service.initialised) {
        return;
    }
    time_sync_on_sntp(&service.sync, epoch);
}
