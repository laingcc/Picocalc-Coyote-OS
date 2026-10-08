#include <stdio.h>
#include <string.h>

#include "ai/wifi_scan.h"
#include "test_util.h"

/* Fake scan adapter: a settable clock and a radio that can refuse to scan. */
typedef struct {
    uint32_t now_ms;
    int start_result;
    int start_calls;
    /* When set, reported from inside scan_start as some drivers do. */
    const char *immediate_ssid;
    wifi_scan_t *scan;
} fake_radio_t;

static uint32_t fake_clock_ms(void *context) {
    return ((fake_radio_t *)context)->now_ms;
}

static int fake_scan_start(void *context) {
    fake_radio_t *radio = (fake_radio_t *)context;
    radio->start_calls++;
    if (radio->immediate_ssid != NULL) {
        wifi_scan_on_result(radio->scan, radio->immediate_ssid, -40);
    }
    return radio->start_result;
}

static const wifi_scan_ops_t fake_ops = {fake_clock_ms, fake_scan_start};

static wifi_scan_t scan;
static fake_radio_t radio;

static void setup(void) {
    memset(&radio, 0, sizeof(radio));
    radio.now_ms = 1000u;
    radio.scan = &scan;
    wifi_scan_init(&scan, &fake_ops, &radio);
}

static void check_initial_state(void) {
    setup();
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_IDLE);
    CHECK(wifi_scan_count(&scan) == 0u);
    CHECK(wifi_scan_ssid(&scan, 0u) == NULL);
    CHECK(wifi_scan_rssi(&scan, 0u) == 0);
    CHECK(radio.start_calls == 0);

    /* Nothing is collected, completed or timed out before a scan starts. */
    wifi_scan_on_result(&scan, "Early", -50);
    wifi_scan_on_done(&scan);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS * 2u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_IDLE);
    CHECK(wifi_scan_count(&scan) == 0u);
}

static void check_start_failure(void) {
    setup();
    radio.start_result = -1;
    CHECK(wifi_scan_start(&scan) == -1);
    CHECK(radio.start_calls == 1);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* A failed scan takes no results and does not complete. */
    wifi_scan_on_result(&scan, "Late", -50);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* Whatever arrived before the failure was reported is dropped too. */
    radio.immediate_ssid = "Glimpse";
    CHECK(wifi_scan_start(&scan) == -1);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* The next attempt can succeed. */
    radio.start_result = 0;
    radio.immediate_ssid = NULL;
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(radio.start_calls == 3);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
}

static void check_done_lists_results(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    wifi_scan_on_result(&scan, "Cafe", -70);
    wifi_scan_on_result(&scan, "Home Net", -45);
    wifi_scan_on_result(&scan, "Office", -82);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    CHECK(wifi_scan_count(&scan) == 3u);
    wifi_scan_poll(&scan);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);

    /* Strongest first. */
    CHECK(wifi_scan_count(&scan) == 3u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Home Net");
    CHECK(wifi_scan_rssi(&scan, 0u) == -45);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 1u), "Cafe");
    CHECK(wifi_scan_rssi(&scan, 1u) == -70);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 2u), "Office");
    CHECK(wifi_scan_rssi(&scan, 2u) == -82);
    CHECK(wifi_scan_ssid(&scan, 3u) == NULL);
    CHECK(wifi_scan_rssi(&scan, 3u) == 0);

    /* The list is final: late results and a late timeout change nothing. */
    wifi_scan_on_result(&scan, "Straggler", -30);
    wifi_scan_on_done(&scan);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS * 2u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == 3u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Home Net");
}

static void check_empty_scan(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == 0u);
}

static void check_result_inside_start(void) {
    setup();
    radio.immediate_ssid = "Instant";
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(wifi_scan_count(&scan) == 1u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Instant");
}

static void check_dedup_keeps_strongest(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_result(&scan, "Mesh", -70);
    wifi_scan_on_result(&scan, "Other", -60);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Other");

    /* A stronger sighting replaces the entry and moves it up. */
    wifi_scan_on_result(&scan, "Mesh", -50);
    CHECK(wifi_scan_count(&scan) == 2u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Mesh");
    CHECK(wifi_scan_rssi(&scan, 0u) == -50);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 1u), "Other");
    CHECK(wifi_scan_rssi(&scan, 1u) == -60);

    /* A weaker or equal sighting is ignored. */
    wifi_scan_on_result(&scan, "Mesh", -90);
    wifi_scan_on_result(&scan, "Mesh", -50);
    wifi_scan_on_result(&scan, "Other", -61);
    CHECK(wifi_scan_count(&scan) == 2u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Mesh");
    CHECK(wifi_scan_rssi(&scan, 0u) == -50);
    CHECK(wifi_scan_rssi(&scan, 1u) == -60);

    /* SSIDs are compared exactly: case and prefixes are distinct networks. */
    wifi_scan_on_result(&scan, "mesh", -55);
    wifi_scan_on_result(&scan, "Mes", -56);
    CHECK(wifi_scan_count(&scan) == 4u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 1u), "mesh");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 2u), "Mes");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 3u), "Other");

    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == 4u);
}

static void check_rejected_results(void) {
    char longest[WIFI_SCAN_SSID_CAPACITY];
    char too_long[WIFI_SCAN_SSID_CAPACITY + 1u];

    memset(longest, 'a', sizeof(longest) - 1u);
    longest[sizeof(longest) - 1u] = '\0';
    memset(too_long, 'b', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';

    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_result(&scan, NULL, -40);
    wifi_scan_on_result(&scan, "", -40); /* hidden network */
    wifi_scan_on_result(&scan, too_long, -40);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* A full 32-character SSID fits. */
    wifi_scan_on_result(&scan, longest, -40);
    CHECK(wifi_scan_count(&scan) == 1u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), longest);

    /* Signal strengths outside int16_t are clamped, not wrapped. */
    wifi_scan_on_result(&scan, "Far", -100000);
    wifi_scan_on_result(&scan, "Near", 100000);
    CHECK(wifi_scan_count(&scan) == 3u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "Near");
    CHECK(wifi_scan_rssi(&scan, 0u) == INT16_MAX);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 2u), "Far");
    CHECK(wifi_scan_rssi(&scan, 2u) == INT16_MIN);
}

static void check_cap(void) {
    char name[16];
    size_t i;

    setup();
    CHECK(WIFI_SCAN_MAX_NETWORKS == 16u);
    CHECK(wifi_scan_start(&scan) == 0);
    /* net0 (-40) ... net15 (-55) fill the list. */
    for (i = 0u; i < WIFI_SCAN_MAX_NETWORKS; i++) {
        snprintf(name, sizeof(name), "net%u", (unsigned)i);
        wifi_scan_on_result(&scan, name, -40 - (int)i);
        CHECK(wifi_scan_count(&scan) == i + 1u);
    }

    /* Weaker than, or only as strong as, everything listed: dropped. */
    wifi_scan_on_result(&scan, "weak", -90);
    wifi_scan_on_result(&scan, "tie", -55);
    CHECK(wifi_scan_count(&scan) == WIFI_SCAN_MAX_NETWORKS);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "net0");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 15u), "net15");
    CHECK(wifi_scan_ssid(&scan, 16u) == NULL);

    /* Stronger than the weakest: it takes that one's place. */
    wifi_scan_on_result(&scan, "strong", -30);
    CHECK(wifi_scan_count(&scan) == WIFI_SCAN_MAX_NETWORKS);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "strong");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 1u), "net0");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 15u), "net14");
    CHECK(wifi_scan_rssi(&scan, 15u) == -54);

    /* A stronger sighting of a listed network reorders without evicting. */
    wifi_scan_on_result(&scan, "net14", -20);
    CHECK(wifi_scan_count(&scan) == WIFI_SCAN_MAX_NETWORKS);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "net14");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 1u), "strong");
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 15u), "net13");

    /* Every entry is still unique and in order. */
    for (i = 1u; i < WIFI_SCAN_MAX_NETWORKS; i++) {
        size_t j;
        CHECK(wifi_scan_rssi(&scan, i - 1u) >= wifi_scan_rssi(&scan, i));
        for (j = 0u; j < i; j++) {
            CHECK(strcmp(wifi_scan_ssid(&scan, i), wifi_scan_ssid(&scan, j)) != 0);
        }
    }

    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == WIFI_SCAN_MAX_NETWORKS);
}

static void check_timeout(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_result(&scan, "Partial", -50);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS - 1u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    CHECK(wifi_scan_count(&scan) == 1u);

    radio.now_ms += 1u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    CHECK(wifi_scan_count(&scan) == 0u);
    CHECK(wifi_scan_ssid(&scan, 0u) == NULL);

    /* The radio finishing late does not revive the scan. */
    wifi_scan_on_result(&scan, "Late", -50);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* The timeout is measured across a clock wrap. */
    setup();
    radio.now_ms = UINT32_MAX - 100u;
    CHECK(wifi_scan_start(&scan) == 0);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS - 1u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    radio.now_ms += 1u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);

    /* Each scan gets the full timeout. */
    radio.now_ms += 500000u;
    CHECK(wifi_scan_start(&scan) == 0);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS - 1u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
}

static void check_start_while_scanning(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_result(&scan, "Kept", -50);
    radio.now_ms += 5000u;

    /* Refused without disturbing the scan in progress or its deadline. */
    CHECK(wifi_scan_start(&scan) == -1);
    CHECK(radio.start_calls == 1);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    CHECK(wifi_scan_count(&scan) == 1u);
    radio.now_ms += WIFI_SCAN_TIMEOUT_MS - 5000u;
    wifi_scan_poll(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
}

static void check_rescan_after_reset(void) {
    setup();
    CHECK(wifi_scan_start(&scan) == 0);
    wifi_scan_on_result(&scan, "Old One", -50);
    wifi_scan_on_result(&scan, "Old Two", -60);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == 2u);

    wifi_scan_reset(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_IDLE);
    CHECK(wifi_scan_count(&scan) == 0u);
    CHECK(wifi_scan_ssid(&scan, 0u) == NULL);
    wifi_scan_on_result(&scan, "Ignored", -40);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* The new scan starts empty and lists only what it finds. */
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(radio.start_calls == 2);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
    CHECK(wifi_scan_count(&scan) == 0u);
    wifi_scan_on_result(&scan, "New", -65);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_DONE);
    CHECK(wifi_scan_count(&scan) == 1u);
    CHECK_STR_EQ(wifi_scan_ssid(&scan, 0u), "New");
    CHECK(wifi_scan_rssi(&scan, 0u) == -65);

    /* Starting again without a reset also discards the old list. */
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* Reset abandons a scan in progress; its completion is ignored. */
    wifi_scan_on_result(&scan, "Abandoned", -50);
    wifi_scan_reset(&scan);
    wifi_scan_on_done(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_IDLE);
    CHECK(wifi_scan_count(&scan) == 0u);

    /* Reset clears an error. */
    radio.start_result = -1;
    CHECK(wifi_scan_start(&scan) == -1);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_ERROR);
    wifi_scan_reset(&scan);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_IDLE);
    radio.start_result = 0;
    CHECK(wifi_scan_start(&scan) == 0);
    CHECK(wifi_scan_state(&scan) == WIFI_SCAN_SCANNING);
}

void test_wifi_scan(void) {
    check_initial_state();
    check_start_failure();
    check_done_lists_results();
    check_empty_scan();
    check_result_inside_start();
    check_dedup_keeps_strongest();
    check_rejected_results();
    check_cap();
    check_timeout();
    check_start_while_scanning();
    check_rescan_after_reset();
}
