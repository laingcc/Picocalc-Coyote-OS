#include "ai/wifi_scan.h"

#include <string.h>

static void clear_results(wifi_scan_t *scan) {
    scan->count = 0u;
    memset(scan->networks, 0, sizeof(scan->networks));
}

static void remove_at(wifi_scan_t *scan, size_t index) {
    memmove(&scan->networks[index], &scan->networks[index + 1u],
            (scan->count - index - 1u) * sizeof(scan->networks[0]));
    scan->count--;
}

void wifi_scan_init(wifi_scan_t *scan, const wifi_scan_ops_t *ops, void *ops_context) {
    memset(scan, 0, sizeof(*scan));
    scan->ops = ops;
    scan->ops_context = ops_context;
    scan->state = WIFI_SCAN_IDLE;
}

int wifi_scan_start(wifi_scan_t *scan) {
    if (scan->state == WIFI_SCAN_SCANNING) {
        return -1;
    }
    clear_results(scan);
    /* SCANNING before the call: the platform may report from inside it. */
    scan->since_ms = scan->ops->clock_ms(scan->ops_context);
    scan->state = WIFI_SCAN_SCANNING;
    if (scan->ops->scan_start(scan->ops_context) != 0) {
        clear_results(scan);
        scan->state = WIFI_SCAN_ERROR;
        return -1;
    }
    return 0;
}

void wifi_scan_reset(wifi_scan_t *scan) {
    clear_results(scan);
    scan->state = WIFI_SCAN_IDLE;
}

void wifi_scan_on_result(wifi_scan_t *scan, const char *ssid, int rssi) {
    size_t length, i, at;

    if (scan->state != WIFI_SCAN_SCANNING || ssid == NULL) {
        return;
    }
    length = strlen(ssid);
    if (length == 0u || length >= WIFI_SCAN_SSID_CAPACITY) {
        return;
    }
    if (rssi < INT16_MIN) {
        rssi = INT16_MIN;
    } else if (rssi > INT16_MAX) {
        rssi = INT16_MAX;
    }

    /* An access point is usually reported more than once, and one network can
     * have several: keep a single entry with the strongest signal. */
    for (i = 0u; i < scan->count; i++) {
        if (strcmp(scan->networks[i].ssid, ssid) == 0) {
            if (rssi <= scan->networks[i].rssi) {
                return;
            }
            remove_at(scan, i);
            break;
        }
    }

    /* Insert in order, strongest first; a full list gives up its weakest. */
    for (at = 0u; at < scan->count && scan->networks[at].rssi >= rssi; at++) {
    }
    if (at == WIFI_SCAN_MAX_NETWORKS) {
        return;
    }
    if (scan->count == WIFI_SCAN_MAX_NETWORKS) {
        scan->count--;
    }
    memmove(&scan->networks[at + 1u], &scan->networks[at], (scan->count - at) * sizeof(scan->networks[0]));
    memset(&scan->networks[at], 0, sizeof(scan->networks[at]));
    memcpy(scan->networks[at].ssid, ssid, length);
    scan->networks[at].rssi = (int16_t)rssi;
    scan->count++;
}

void wifi_scan_on_done(wifi_scan_t *scan) {
    if (scan->state == WIFI_SCAN_SCANNING) {
        scan->state = WIFI_SCAN_DONE;
    }
}

void wifi_scan_poll(wifi_scan_t *scan) {
    if (scan->state != WIFI_SCAN_SCANNING) {
        return;
    }
    /* Unsigned subtraction keeps the comparison right across a clock wrap. */
    if (scan->ops->clock_ms(scan->ops_context) - scan->since_ms >= WIFI_SCAN_TIMEOUT_MS) {
        clear_results(scan);
        scan->state = WIFI_SCAN_ERROR;
    }
}

wifi_scan_state_t wifi_scan_state(const wifi_scan_t *scan) {
    return scan->state;
}

size_t wifi_scan_count(const wifi_scan_t *scan) {
    return scan->count;
}

const char *wifi_scan_ssid(const wifi_scan_t *scan, size_t index) {
    return index < scan->count ? scan->networks[index].ssid : NULL;
}

int wifi_scan_rssi(const wifi_scan_t *scan, size_t index) {
    return index < scan->count ? scan->networks[index].rssi : 0;
}
