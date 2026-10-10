#include "time/time_sync.h"

#include <string.h>

#define RECORD_KEY "epoch="
#define RECORD_KEY_LENGTH (sizeof(RECORD_KEY) - 1u)

static uint32_t now_ms(const time_sync_t *sync) {
    return sync->ops->clock_ms(sync->ops_context);
}

/* Unsigned subtraction keeps this right across a wrap of the clock. */
static uint32_t epoch_at(const time_sync_t *sync, uint32_t ms) {
    return sync->epoch + (ms - sync->base_ms) / 1000u;
}

/* Fold the whole seconds that have passed into the epoch, so the span measured
 * from base_ms stays far below the clock's wrap. */
static void advance(time_sync_t *sync, uint32_t ms) {
    uint32_t seconds = (ms - sync->base_ms) / 1000u;

    sync->epoch += seconds;
    sync->base_ms += seconds * 1000u;
}

void time_sync_init(time_sync_t *sync, const time_sync_ops_t *ops, void *ops_context) {
    uint32_t saved = 0u;

    memset(sync, 0, sizeof(*sync));
    sync->ops = ops;
    sync->ops_context = ops_context;
    sync->base_ms = now_ms(sync);
    sync->saved_ms = sync->base_ms;
    if (ops->load(ops_context, &saved) == 0 && saved >= TIME_SYNC_EPOCH_MIN) {
        sync->epoch = saved;
        sync->state = TIME_SYNC_RESTORED;
    }
}

void time_sync_poll(time_sync_t *sync, bool online) {
    uint32_t ms = now_ms(sync);

    if (online && !sync->sntp_running) {
        sync->sntp_running = true;
        sync->ops->sntp_start(sync->ops_context);
    } else if (!online && sync->sntp_running) {
        sync->sntp_running = false;
        sync->ops->sntp_stop(sync->ops_context);
    }

    if (sync->state == TIME_SYNC_UNSET) {
        return;
    }
    advance(sync, ms);
    if (sync->save_pending || ms - sync->saved_ms >= TIME_SYNC_SAVE_INTERVAL_MS) {
        /* A failed save is not retried until the next one is due: with no
         * card in the slot it would otherwise run on every poll. */
        sync->save_pending = false;
        sync->saved_ms = ms;
        sync->ops->save(sync->ops_context, sync->epoch);
    }
}

void time_sync_on_sntp(time_sync_t *sync, uint32_t epoch) {
    if (epoch < TIME_SYNC_EPOCH_MIN) {
        return;
    }
    sync->epoch = epoch;
    sync->base_ms = now_ms(sync);
    sync->state = TIME_SYNC_SYNCED;
    sync->save_pending = true;
}

time_sync_state_t time_sync_state(const time_sync_t *sync) {
    return sync->state;
}

bool time_sync_now(const time_sync_t *sync, uint32_t *epoch) {
    if (sync->state == TIME_SYNC_UNSET) {
        return false;
    }
    *epoch = epoch_at(sync, now_ms(sync));
    return true;
}

int time_sync_format_hhmm(uint32_t epoch, char *buffer, size_t capacity) {
    uint32_t minutes = epoch / 60u % (24u * 60u);

    if (buffer == NULL || capacity < TIME_SYNC_HHMM_CAPACITY) {
        if (buffer != NULL && capacity > 0u) {
            buffer[0] = '\0';
        }
        return -1;
    }
    buffer[0] = (char)('0' + minutes / 600u);
    buffer[1] = (char)('0' + minutes / 60u % 10u);
    buffer[2] = ':';
    buffer[3] = (char)('0' + minutes % 60u / 10u);
    buffer[4] = (char)('0' + minutes % 10u);
    buffer[5] = '\0';
    return 0;
}

int time_sync_label(const time_sync_t *sync, int utc_offset_minutes, char *buffer, size_t capacity) {
    /* The offset as minutes ahead of UTC within one day, so it can be added
     * to the time of day without going negative or overflowing the epoch. */
    uint32_t ahead = (uint32_t)((utc_offset_minutes % (24 * 60) + 24 * 60) % (24 * 60));
    uint32_t epoch;

    if (buffer == NULL || capacity < TIME_SYNC_LABEL_CAPACITY) {
        if (buffer != NULL && capacity > 0u) {
            buffer[0] = '\0';
        }
        return -1;
    }
    if (!time_sync_now(sync, &epoch)) {
        memcpy(buffer, "--:--", 6u);
        return 0;
    }
    time_sync_format_hhmm(epoch % (24u * 60u * 60u) + ahead * 60u, buffer, capacity);
    if (sync->state == TIME_SYNC_RESTORED) {
        memcpy(buffer + 5, "?", 2u);
    }
    return 0;
}

int time_sync_record_format(uint32_t epoch, char *buffer, size_t capacity) {
    char digits[10];
    size_t count = 0u;
    size_t length = RECORD_KEY_LENGTH;

    if (buffer == NULL || capacity < TIME_SYNC_RECORD_CAPACITY) {
        return -1;
    }
    do {
        digits[count++] = (char)('0' + epoch % 10u);
        epoch /= 10u;
    } while (epoch != 0u);

    memcpy(buffer, RECORD_KEY, RECORD_KEY_LENGTH);
    while (count > 0u) {
        buffer[length++] = digits[--count];
    }
    buffer[length++] = '\n';
    buffer[length] = '\0';
    return (int)length;
}

int time_sync_record_parse(const char *text, size_t length, uint32_t *epoch) {
    uint32_t value = 0u;
    size_t i = RECORD_KEY_LENGTH;

    if (text == NULL || epoch == NULL || length <= RECORD_KEY_LENGTH ||
        memcmp(text, RECORD_KEY, RECORD_KEY_LENGTH) != 0) {
        return -1;
    }
    if (text[length - 1u] == '\n') {
        length--;
        if (text[length - 1u] == '\r') {
            length--;
        }
    }
    if (i == length) {
        return -1;
    }
    for (; i < length; i++) {
        uint32_t digit;

        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        digit = (uint32_t)(text[i] - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return -1;
        }
        value = value * 10u + digit;
    }
    if (value < TIME_SYNC_EPOCH_MIN) {
        return -1;
    }
    *epoch = value;
    return 0;
}
