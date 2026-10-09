#include <stdint.h>
#include <string.h>

#include "ai/time_sync.h"
#include "test_util.h"

/* 2026-10-09T14:05:09Z */
#define T0 1791554709u

/* Fake platform: a settable clock, a one-slot store that can fail, and an SNTP
 * client that only counts its starts and stops. */
typedef struct {
    uint32_t now_ms;
    int sntp_starts;
    int sntp_stops;
    int has_saved;
    uint32_t saved;
    int load_calls;
    int save_calls;
    int save_result;
} fake_platform_t;

static uint32_t fake_clock_ms(void *context) {
    return ((fake_platform_t *)context)->now_ms;
}

static void fake_sntp_start(void *context) {
    ((fake_platform_t *)context)->sntp_starts++;
}

static void fake_sntp_stop(void *context) {
    ((fake_platform_t *)context)->sntp_stops++;
}

static int fake_load(void *context, uint32_t *epoch) {
    fake_platform_t *platform = (fake_platform_t *)context;
    platform->load_calls++;
    if (!platform->has_saved) {
        return -1;
    }
    *epoch = platform->saved;
    return 0;
}

static int fake_save(void *context, uint32_t epoch) {
    fake_platform_t *platform = (fake_platform_t *)context;
    platform->save_calls++;
    if (platform->save_result != 0) {
        return platform->save_result;
    }
    platform->has_saved = 1;
    platform->saved = epoch;
    return 0;
}

static const time_sync_ops_t fake_ops = {fake_clock_ms, fake_sntp_start, fake_sntp_stop, fake_load, fake_save};

static void test_format_hhmm(void) {
    char text[TIME_SYNC_HHMM_CAPACITY];
    char small[TIME_SYNC_HHMM_CAPACITY - 1u] = "xxxx";

    CHECK(time_sync_format_hhmm(0u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "00:00");
    CHECK(time_sync_format_hhmm(T0, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "14:05");
    /* Seconds are dropped, not rounded. */
    CHECK(time_sync_format_hhmm(59u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "00:00");
    CHECK(time_sync_format_hhmm(60u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "00:01");
    CHECK(time_sync_format_hhmm(86399u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "23:59");
    CHECK(time_sync_format_hhmm(86400u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "00:00");
    /* 2038-01-19T03:14:08Z, one past INT32_MAX, and the last second of all. */
    CHECK(time_sync_format_hhmm(2147483648u, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "03:14");
    CHECK(time_sync_format_hhmm(UINT32_MAX, text, sizeof(text)) == 0);
    CHECK_STR_EQ(text, "06:28");

    CHECK(time_sync_format_hhmm(T0, small, sizeof(small)) == -1);
    CHECK_STR_EQ(small, "");
    CHECK(time_sync_format_hhmm(T0, NULL, 0u) == -1);
}

static void test_record_round_trip(void) {
    char record[TIME_SYNC_RECORD_CAPACITY];
    uint32_t epoch = 0u;

    CHECK(time_sync_record_format(T0, record, sizeof(record)) == 17);
    CHECK_STR_EQ(record, "epoch=1791554709\n");
    CHECK(time_sync_record_parse(record, strlen(record), &epoch) == 0);
    CHECK(epoch == T0);

    /* The longest record exactly fills the buffer. */
    CHECK(time_sync_record_format(UINT32_MAX, record, sizeof(record)) == 17);
    CHECK_STR_EQ(record, "epoch=4294967295\n");
    CHECK(time_sync_record_parse(record, strlen(record), &epoch) == 0);
    CHECK(epoch == UINT32_MAX);

    CHECK(time_sync_record_format(0u, record, sizeof(record)) == 8);
    CHECK_STR_EQ(record, "epoch=0\n");

    CHECK(time_sync_record_format(T0, record, sizeof(record) - 1u) == -1);
    CHECK(time_sync_record_format(T0, NULL, sizeof(record)) == -1);
}

static int parse(const char *text, uint32_t *epoch) {
    return time_sync_record_parse(text, strlen(text), epoch);
}

static void test_record_parse(void) {
    uint32_t epoch = 7u;

    CHECK(parse("epoch=1791554709", &epoch) == 0);
    CHECK(epoch == T0);
    epoch = 7u;
    CHECK(parse("epoch=1791554709\r\n", &epoch) == 0);
    CHECK(epoch == T0);
    CHECK(parse("epoch=1735689600\n", &epoch) == 0);
    CHECK(epoch == TIME_SYNC_EPOCH_MIN);

    /* Rejected input leaves *epoch alone. */
    epoch = 7u;
    CHECK(parse("", &epoch) == -1);
    CHECK(parse("\n", &epoch) == -1);
    CHECK(parse("epoch=", &epoch) == -1);
    CHECK(parse("epoch=\n", &epoch) == -1);
    CHECK(parse("epoch=\r\n", &epoch) == -1);
    CHECK(parse("epoch=1735689599\n", &epoch) == -1); /* before the floor */
    CHECK(parse("epoch=0\n", &epoch) == -1);
    CHECK(parse("epoch=4294967296\n", &epoch) == -1); /* one past UINT32_MAX */
    CHECK(parse("epoch=99999999999999999999\n", &epoch) == -1);
    CHECK(parse("epoch=-1791554709\n", &epoch) == -1);
    CHECK(parse("epoch= 1791554709\n", &epoch) == -1);
    CHECK(parse("epoch=1791554709 \n", &epoch) == -1);
    CHECK(parse("epoch=17915x4709\n", &epoch) == -1);
    CHECK(parse("epoch=1791554709\n\n", &epoch) == -1);
    CHECK(parse("EPOCH=1791554709\n", &epoch) == -1);
    CHECK(parse("time=1791554709\n", &epoch) == -1);
    CHECK(parse("1791554709\n", &epoch) == -1);
    CHECK(epoch == 7u);

    /* Only length bytes are read: a save cut short does not parse as less. */
    CHECK(time_sync_record_parse("epoch=1791554709\n", 6u, &epoch) == -1);
    CHECK(time_sync_record_parse("epoch=1791554709\n", 15u, &epoch) == -1);
    CHECK(time_sync_record_parse(NULL, 4u, &epoch) == -1);
    CHECK(time_sync_record_parse("epoch=1791554709\n", 17u, NULL) == -1);
}

static void test_starts_unset(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    char label[TIME_SYNC_LABEL_CAPACITY];
    uint32_t epoch = 7u;

    platform.now_ms = 5000u;
    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(platform.load_calls == 1);
    CHECK(time_sync_state(&sync) == TIME_SYNC_UNSET);
    CHECK(!time_sync_now(&sync, &epoch));
    CHECK(epoch == 7u);
    CHECK(time_sync_label(&sync, label, sizeof(label)) == 0);
    CHECK_STR_EQ(label, "--:--");

    /* With nothing to save, time passing offline does nothing at all. */
    platform.now_ms += 3u * TIME_SYNC_SAVE_INTERVAL_MS;
    time_sync_poll(&sync, false);
    CHECK(platform.save_calls == 0);
    CHECK(platform.sntp_starts == 0);
    CHECK(platform.sntp_stops == 0);
    CHECK(time_sync_state(&sync) == TIME_SYNC_UNSET);
}

static void test_sntp_follows_link(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;

    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_poll(&sync, false);
    CHECK(platform.sntp_starts == 0);

    time_sync_poll(&sync, true);
    time_sync_poll(&sync, true);
    CHECK(platform.sntp_starts == 1);
    CHECK(platform.sntp_stops == 0);

    time_sync_poll(&sync, false);
    time_sync_poll(&sync, false);
    CHECK(platform.sntp_starts == 1);
    CHECK(platform.sntp_stops == 1);

    /* Every reconnect starts the client again, which asks straight away. */
    time_sync_poll(&sync, true);
    CHECK(platform.sntp_starts == 2);
    CHECK(platform.sntp_stops == 1);
}

static void test_sync_sets_time_and_saves(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    char label[TIME_SYNC_LABEL_CAPACITY];
    uint32_t epoch = 0u;

    platform.now_ms = 1000u;
    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_poll(&sync, true);

    platform.now_ms = 4200u;
    time_sync_on_sntp(&sync, T0);
    CHECK(time_sync_state(&sync) == TIME_SYNC_SYNCED);
    /* The answer arrives in a network callback: nothing is written there. */
    CHECK(platform.save_calls == 0);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0);
    CHECK(time_sync_label(&sync, label, sizeof(label)) == 0);
    CHECK_STR_EQ(label, "14:05");

    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);
    CHECK(platform.saved == T0);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);

    /* The clock runs on between answers. */
    platform.now_ms += 999u;
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0);
    platform.now_ms += 1u;
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 1u);
    platform.now_ms += 51000u;
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 52u);
    CHECK(time_sync_label(&sync, label, sizeof(label)) == 0);
    CHECK_STR_EQ(label, "14:06");

    /* A later answer replaces the time, even with an earlier one. */
    time_sync_on_sntp(&sync, T0 + 40u);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 40u);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 2);
    CHECK(platform.saved == T0 + 40u);
}

static void test_implausible_answer_is_ignored(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    uint32_t epoch = 0u;

    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_on_sntp(&sync, 0u);
    time_sync_on_sntp(&sync, TIME_SYNC_EPOCH_MIN - 1u);
    CHECK(time_sync_state(&sync) == TIME_SYNC_UNSET);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 0);

    time_sync_on_sntp(&sync, T0);
    time_sync_on_sntp(&sync, 12u);
    CHECK(time_sync_state(&sync) == TIME_SYNC_SYNCED);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0);

    time_sync_on_sntp(&sync, TIME_SYNC_EPOCH_MIN);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == TIME_SYNC_EPOCH_MIN);
}

static void test_periodic_save(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;

    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_on_sntp(&sync, T0);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);

    platform.now_ms += TIME_SYNC_SAVE_INTERVAL_MS - 1u;
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);
    platform.now_ms += 1u;
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 2);
    CHECK(platform.saved == T0 + TIME_SYNC_SAVE_INTERVAL_MS / 1000u);

    /* Saving carries on offline: the clock still runs. */
    platform.now_ms += TIME_SYNC_SAVE_INTERVAL_MS;
    time_sync_poll(&sync, false);
    CHECK(platform.save_calls == 3);
    CHECK(platform.saved == T0 + 2u * TIME_SYNC_SAVE_INTERVAL_MS / 1000u);
}

static void test_failed_save_waits_for_next_interval(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    uint32_t epoch = 0u;

    platform.save_result = -1;
    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_on_sntp(&sync, T0);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);
    CHECK(!platform.has_saved);

    /* No card: polling does not hammer the store. */
    platform.now_ms += 20u;
    time_sync_poll(&sync, true);
    platform.now_ms += TIME_SYNC_SAVE_INTERVAL_MS - 21u;
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);
    CHECK(time_sync_state(&sync) == TIME_SYNC_SYNCED);
    CHECK(time_sync_now(&sync, &epoch));

    platform.save_result = 0;
    platform.now_ms += 1u;
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 2);
    CHECK(platform.saved == T0 + TIME_SYNC_SAVE_INTERVAL_MS / 1000u);
}

static void test_restores_saved_time(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    char label[TIME_SYNC_LABEL_CAPACITY];
    uint32_t epoch = 0u;

    platform.has_saved = 1;
    platform.saved = T0;
    platform.now_ms = 800u;
    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(time_sync_state(&sync) == TIME_SYNC_RESTORED);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0);
    CHECK(time_sync_label(&sync, label, sizeof(label)) == 0);
    CHECK_STR_EQ(label, "14:05?");

    /* It drifts on from where it was, and is not rewritten until due. */
    time_sync_poll(&sync, false);
    CHECK(platform.save_calls == 0);
    platform.now_ms += 120000u;
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 120u);
    platform.now_ms += TIME_SYNC_SAVE_INTERVAL_MS - 120000u;
    time_sync_poll(&sync, false);
    CHECK(platform.save_calls == 1);
    CHECK(platform.saved == T0 + TIME_SYNC_SAVE_INTERVAL_MS / 1000u);
    CHECK(time_sync_state(&sync) == TIME_SYNC_RESTORED);

    /* The network corrects it. */
    time_sync_poll(&sync, true);
    time_sync_on_sntp(&sync, T0 + 90000u);
    CHECK(time_sync_state(&sync) == TIME_SYNC_SYNCED);
    CHECK(time_sync_label(&sync, label, sizeof(label)) == 0);
    CHECK_STR_EQ(label, "15:05");
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 2);
    CHECK(platform.saved == T0 + 90000u);
}

static void test_implausible_saved_time_is_ignored(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;

    platform.has_saved = 1;
    platform.saved = TIME_SYNC_EPOCH_MIN - 1u;
    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(time_sync_state(&sync) == TIME_SYNC_UNSET);

    platform.saved = TIME_SYNC_EPOCH_MIN;
    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(time_sync_state(&sync) == TIME_SYNC_RESTORED);
}

static void test_survives_reboot(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    uint32_t epoch = 0u;

    platform.now_ms = 60000u;
    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_poll(&sync, true);
    time_sync_on_sntp(&sync, T0);
    time_sync_poll(&sync, true);
    platform.now_ms += TIME_SYNC_SAVE_INTERVAL_MS + 30000u;
    time_sync_poll(&sync, true);

    /* Reboot: the monotonic clock starts over, the store does not. */
    platform.now_ms = 0u;
    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(time_sync_state(&sync) == TIME_SYNC_RESTORED);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + (TIME_SYNC_SAVE_INTERVAL_MS + 30000u) / 1000u);
}

static void test_clock_wrap(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    uint32_t epoch = 0u;
    uint32_t elapsed = 0u;
    int i;

    /* Synced 2.5 s before the millisecond clock wraps. */
    platform.now_ms = UINT32_MAX - 2499u;
    time_sync_init(&sync, &fake_ops, &platform);
    time_sync_on_sntp(&sync, T0);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);

    platform.now_ms += 4000u; /* wraps to 1500 */
    CHECK(platform.now_ms == 1500u);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 4u);
    time_sync_poll(&sync, true);
    CHECK(platform.save_calls == 1);
    CHECK(time_sync_now(&sync, &epoch));
    CHECK(epoch == T0 + 4u);

    /* Polled at an awkward period for more than two whole wraps (~124 days),
     * no second is lost or counted twice. */
    elapsed = 4u;
    for (i = 0; i < 12000; i++) {
        platform.now_ms += 893777u;
        time_sync_poll(&sync, true);
    }
    CHECK(time_sync_now(&sync, &epoch));
    /* 12000 * 893777 ms = 10725324 s exactly. */
    CHECK(epoch == T0 + elapsed + 10725324u);
    CHECK(time_sync_state(&sync) == TIME_SYNC_SYNCED);
}

static void test_label_capacity(void) {
    fake_platform_t platform = {0};
    time_sync_t sync;
    char small[TIME_SYNC_LABEL_CAPACITY - 1u] = "xxxxx";

    time_sync_init(&sync, &fake_ops, &platform);
    CHECK(time_sync_label(&sync, small, sizeof(small)) == -1);
    CHECK_STR_EQ(small, "");
    CHECK(time_sync_label(&sync, NULL, 0u) == -1);
}

void test_time_sync(void) {
    test_format_hhmm();
    test_record_round_trip();
    test_record_parse();
    test_starts_unset();
    test_sntp_follows_link();
    test_sync_sets_time_and_saves();
    test_implausible_answer_is_ignored();
    test_periodic_save();
    test_failed_save_waits_for_next_interval();
    test_restores_saved_time();
    test_implausible_saved_time_is_ignored();
    test_survives_reboot();
    test_clock_wrap();
    test_label_capacity();
}
