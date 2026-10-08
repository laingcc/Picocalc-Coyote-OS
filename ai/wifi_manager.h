#ifndef COYOTE_AI_WIFI_MANAGER_H
#define COYOTE_AI_WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wi-Fi station state machine.
 *
 * wifi_manager_t decides when to power the radio, when to join, and how long
 * to wait between attempts.  It never touches the radio directly: the platform
 * supplies wifi_manager_ops_t, so the same code drives CYW43 in firmware and a
 * fake in the host tests.  This file and wifi_manager.c include no CYW43, lwIP
 * or Pico SDK headers.
 *
 *   OFF           disabled; the radio is not powered
 *   NEEDS_CONFIG  enabled but no SSID is configured; the radio is not powered
 *   CONNECTING    a join is in progress
 *   ONLINE        associated with an IP address
 *   BACKOFF       the last attempt failed or the link dropped; waiting to retry
 *   ERROR         retrying cannot help (radio failure, rejected credentials);
 *                 stays here until re-enabled or reconfigured
 *
 * Retries are unlimited but the wait between them is bounded: it starts at
 * WIFI_BACKOFF_BASE_MS, doubles after every consecutive failure and never
 * exceeds WIFI_BACKOFF_MAX_MS.  Reaching ONLINE resets it.
 *
 * Credentials
 * -----------
 * The manager keeps its own copy of the SSID and password.  The password is
 * handed to ops->join and nowhere else: there is deliberately no accessor for
 * it, and nothing in this module logs.
 */

/* Capacities include the trailing NUL; they match ai_config_t. */
#define WIFI_SSID_CAPACITY 33u
#define WIFI_PASSWORD_CAPACITY 65u

#define WIFI_BACKOFF_BASE_MS 1000u
#define WIFI_BACKOFF_MAX_MS 60000u
#define WIFI_JOIN_TIMEOUT_MS 30000u

typedef enum {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_NEEDS_CONFIG,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_ONLINE,
    WIFI_STATE_BACKOFF,
    WIFI_STATE_ERROR
} wifi_state_t;

typedef enum {
    WIFI_ERROR_NONE = 0,
    WIFI_ERROR_RADIO, /* the radio could not be powered up */
    WIFI_ERROR_AUTH   /* the network rejected the credentials */
} wifi_error_t;

/* Link status as reported by the radio driver. */
typedef enum {
    WIFI_LINK_DOWN = 0,
    WIFI_LINK_JOINING,    /* associating or waiting for an address */
    WIFI_LINK_UP,         /* associated with an IP address */
    WIFI_LINK_FAILED,     /* the join failed */
    WIFI_LINK_NO_NETWORK, /* the SSID was not found */
    WIFI_LINK_BAD_AUTH    /* the credentials were rejected */
} wifi_link_t;

/* Platform adapters.  All are required and are called with the context given
 * to wifi_manager_init. */
typedef struct {
    /* Monotonic milliseconds; may wrap. */
    uint32_t (*clock_ms)(void *context);
    /* Power the radio and enter station mode.  Returns 0 on success. */
    int (*radio_on)(void *context);
    void (*radio_off)(void *context);
    /* Begin joining without blocking.  password is "" for an open network and
     * is only valid for the duration of the call.  Returns 0 if started. */
    int (*join)(void *context, const char *ssid, const char *password);
    void (*leave)(void *context);
    wifi_link_t (*link_status)(void *context);
} wifi_manager_ops_t;

typedef struct {
    wifi_state_t state;
    wifi_error_t error;
    const wifi_manager_ops_t *ops;
    void *ops_context;
    bool enabled;
    bool radio_powered;
    bool joined;       /* join was started and leave has not been called */
    uint8_t failures;  /* consecutive failed attempts, saturating */
    uint32_t backoff_ms;
    uint32_t since_ms; /* when the current CONNECTING/BACKOFF state began */
    char ssid[WIFI_SSID_CAPACITY];
    char password[WIFI_PASSWORD_CAPACITY];
} wifi_manager_t;

void wifi_manager_init(wifi_manager_t *wifi, const wifi_manager_ops_t *ops, void *ops_context);

/*
 * Replace the stored credentials.  Returns -1 (changing nothing) if either
 * string is NULL or does not fit.  If the manager is enabled it leaves the
 * current network and joins with the new credentials, or drops to NEEDS_CONFIG
 * when the SSID is empty.
 */
int wifi_manager_set_credentials(wifi_manager_t *wifi, const char *ssid, const char *password);

/* Start (or, from ERROR, restart) the station.  A no-op while already
 * connecting, online, backing off or waiting for configuration. */
void wifi_manager_enable(wifi_manager_t *wifi);

/* Leave the network and power the radio down. */
void wifi_manager_disable(wifi_manager_t *wifi);

/* Advance the state machine.  Cheap and non-blocking. */
void wifi_manager_poll(wifi_manager_t *wifi);

wifi_state_t wifi_manager_state(const wifi_manager_t *wifi);
wifi_error_t wifi_manager_error(const wifi_manager_t *wifi);
bool wifi_manager_is_online(const wifi_manager_t *wifi);
/* True while the radio is powered and therefore needs servicing. */
bool wifi_manager_radio_powered(const wifi_manager_t *wifi);
/* The wait chosen for the current or most recent BACKOFF, in milliseconds. */
uint32_t wifi_manager_backoff_ms(const wifi_manager_t *wifi);
/* The configured SSID ("" if none).  There is no password accessor. */
const char *wifi_manager_ssid(const wifi_manager_t *wifi);

#endif
