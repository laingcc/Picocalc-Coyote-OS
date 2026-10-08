#ifndef COYOTE_AI_WIFI_SCAN_H
#define COYOTE_AI_WIFI_SCAN_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wi-Fi network scan state machine.
 *
 * wifi_scan_t collects the networks a scan reports into a bounded list.  Like
 * wifi_manager it never touches the radio: the platform supplies
 * wifi_scan_ops_t and pushes what the radio finds with wifi_scan_on_result and
 * wifi_scan_on_done, so the same code drives CYW43 in firmware and a fake in
 * the host tests.  This file and wifi_scan.c include no CYW43, lwIP or Pico
 * SDK headers.
 *
 *   IDLE      no scan has run since init or reset; the list is empty
 *   SCANNING  a scan is in progress; the list is filling
 *   DONE      the scan completed; the list is final
 *   ERROR     the scan could not be started or did not complete within
 *             WIFI_SCAN_TIMEOUT_MS; the list is empty
 *
 * The list holds each SSID once, with the strongest signal seen for it, and
 * is ordered strongest first.  When more than WIFI_SCAN_MAX_NETWORKS are in
 * range the weakest are dropped.  Hidden networks (empty SSID) are skipped.
 */

#define WIFI_SCAN_MAX_NETWORKS 16u
/* Includes the trailing NUL; matches WIFI_SSID_CAPACITY. */
#define WIFI_SCAN_SSID_CAPACITY 33u
#define WIFI_SCAN_TIMEOUT_MS 15000u

typedef enum {
    WIFI_SCAN_IDLE = 0,
    WIFI_SCAN_SCANNING,
    WIFI_SCAN_DONE,
    WIFI_SCAN_ERROR
} wifi_scan_state_t;

/* Platform adapters.  Both are required and are called with the context given
 * to wifi_scan_init. */
typedef struct {
    /* Monotonic milliseconds; may wrap. */
    uint32_t (*clock_ms)(void *context);
    /* Begin scanning without blocking.  Returns 0 if started, -1 if not. */
    int (*scan_start)(void *context);
} wifi_scan_ops_t;

typedef struct {
    char ssid[WIFI_SCAN_SSID_CAPACITY];
    int16_t rssi; /* dBm */
} wifi_scan_network_t;

typedef struct {
    wifi_scan_state_t state;
    const wifi_scan_ops_t *ops;
    void *ops_context;
    uint32_t since_ms; /* when the current scan began */
    size_t count;
    wifi_scan_network_t networks[WIFI_SCAN_MAX_NETWORKS];
} wifi_scan_t;

void wifi_scan_init(wifi_scan_t *scan, const wifi_scan_ops_t *ops, void *ops_context);

/*
 * Discard the previous results and begin a scan.  Returns 0 once it is under
 * way.  Returns -1 if a scan is already in progress (which is left running)
 * or if the platform could not start one (the state becomes ERROR).
 */
int wifi_scan_start(wifi_scan_t *scan);

/* Forget the results and return to IDLE.  Anything the platform still reports
 * for an abandoned scan is ignored. */
void wifi_scan_reset(wifi_scan_t *scan);

/* The platform found a network.  Ignored unless a scan is in progress, and
 * when ssid is NULL, empty or does not fit. */
void wifi_scan_on_result(wifi_scan_t *scan, const char *ssid, int rssi);

/* The platform finished scanning.  Ignored unless a scan is in progress. */
void wifi_scan_on_done(wifi_scan_t *scan);

/* Enforce the scan timeout.  Cheap and non-blocking. */
void wifi_scan_poll(wifi_scan_t *scan);

wifi_scan_state_t wifi_scan_state(const wifi_scan_t *scan);
/* Networks found so far, strongest first. */
size_t wifi_scan_count(const wifi_scan_t *scan);
/* NULL when index is out of range. */
const char *wifi_scan_ssid(const wifi_scan_t *scan, size_t index);
/* Signal strength in dBm; 0 when index is out of range. */
int wifi_scan_rssi(const wifi_scan_t *scan, size_t index);

#endif
