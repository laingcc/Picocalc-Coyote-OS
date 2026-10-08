#include "ai/wifi_manager.h"

#include <string.h>

static uint32_t now_ms(const wifi_manager_t *wifi) {
    return wifi->ops->clock_ms(wifi->ops_context);
}

static void leave_network(wifi_manager_t *wifi) {
    if (wifi->joined) {
        wifi->joined = false;
        wifi->ops->leave(wifi->ops_context);
    }
}

/* Wait before the next attempt: doubles per consecutive failure, capped. */
static void enter_backoff(wifi_manager_t *wifi) {
    uint32_t delay = WIFI_BACKOFF_BASE_MS;
    uint8_t i;

    leave_network(wifi);
    for (i = 0u; i < wifi->failures && delay < WIFI_BACKOFF_MAX_MS; i++) {
        delay = delay >= WIFI_BACKOFF_MAX_MS / 2u ? WIFI_BACKOFF_MAX_MS : delay * 2u;
    }
    if (wifi->failures < UINT8_MAX) {
        wifi->failures++;
    }
    wifi->backoff_ms = delay;
    wifi->since_ms = now_ms(wifi);
    wifi->state = WIFI_STATE_BACKOFF;
}

static void enter_error(wifi_manager_t *wifi, wifi_error_t error) {
    leave_network(wifi);
    wifi->error = error;
    wifi->state = WIFI_STATE_ERROR;
}

static void begin_join(wifi_manager_t *wifi) {
    if (wifi->ops->join(wifi->ops_context, wifi->ssid, wifi->password) != 0) {
        enter_backoff(wifi);
        return;
    }
    wifi->joined = true;
    wifi->since_ms = now_ms(wifi);
    wifi->state = WIFI_STATE_CONNECTING;
}

/* Bring the station up from scratch with the stored credentials. */
static void begin(wifi_manager_t *wifi) {
    wifi->error = WIFI_ERROR_NONE;
    wifi->failures = 0u;
    wifi->backoff_ms = 0u;
    leave_network(wifi);

    if (wifi->ssid[0] == '\0') {
        wifi->state = WIFI_STATE_NEEDS_CONFIG;
        return;
    }
    if (!wifi->radio_powered) {
        if (wifi->ops->radio_on(wifi->ops_context) != 0) {
            enter_error(wifi, WIFI_ERROR_RADIO);
            return;
        }
        wifi->radio_powered = true;
    }
    begin_join(wifi);
}

void wifi_manager_init(wifi_manager_t *wifi, const wifi_manager_ops_t *ops, void *ops_context) {
    if (wifi == NULL) {
        return;
    }
    memset(wifi, 0, sizeof(*wifi));
    wifi->state = WIFI_STATE_OFF;
    wifi->error = WIFI_ERROR_NONE;
    wifi->ops = ops;
    wifi->ops_context = ops_context;
}

int wifi_manager_set_credentials(wifi_manager_t *wifi, const char *ssid, const char *password) {
    size_t ssid_length;
    size_t password_length;

    if (wifi == NULL || ssid == NULL || password == NULL) {
        return -1;
    }
    ssid_length = strlen(ssid);
    password_length = strlen(password);
    if (ssid_length >= sizeof(wifi->ssid) || password_length >= sizeof(wifi->password)) {
        return -1;
    }

    /* Clear first so no tail of an older, longer password lingers. */
    memset(wifi->ssid, 0, sizeof(wifi->ssid));
    memset(wifi->password, 0, sizeof(wifi->password));
    memcpy(wifi->ssid, ssid, ssid_length);
    memcpy(wifi->password, password, password_length);

    if (wifi->enabled) {
        begin(wifi);
    }
    return 0;
}

void wifi_manager_enable(wifi_manager_t *wifi) {
    if (wifi == NULL || wifi->ops == NULL) {
        return;
    }
    if (wifi->enabled && wifi->state != WIFI_STATE_ERROR) {
        return;
    }
    wifi->enabled = true;
    begin(wifi);
}

void wifi_manager_disable(wifi_manager_t *wifi) {
    if (wifi == NULL || wifi->ops == NULL) {
        return;
    }
    leave_network(wifi);
    if (wifi->radio_powered) {
        wifi->radio_powered = false;
        wifi->ops->radio_off(wifi->ops_context);
    }
    wifi->enabled = false;
    wifi->error = WIFI_ERROR_NONE;
    wifi->state = WIFI_STATE_OFF;
}

void wifi_manager_poll(wifi_manager_t *wifi) {
    wifi_link_t link;

    if (wifi == NULL || wifi->ops == NULL) {
        return;
    }
    switch (wifi->state) {
        case WIFI_STATE_CONNECTING:
            link = wifi->ops->link_status(wifi->ops_context);
            if (link == WIFI_LINK_UP) {
                wifi->failures = 0u;
                wifi->state = WIFI_STATE_ONLINE;
            } else if (link == WIFI_LINK_BAD_AUTH) {
                enter_error(wifi, WIFI_ERROR_AUTH);
            } else if (link == WIFI_LINK_FAILED || link == WIFI_LINK_NO_NETWORK) {
                enter_backoff(wifi);
            } else if (now_ms(wifi) - wifi->since_ms >= WIFI_JOIN_TIMEOUT_MS) {
                enter_backoff(wifi);
            }
            break;
        case WIFI_STATE_ONLINE:
            if (wifi->ops->link_status(wifi->ops_context) != WIFI_LINK_UP) {
                enter_backoff(wifi);
            }
            break;
        case WIFI_STATE_BACKOFF:
            if (now_ms(wifi) - wifi->since_ms >= wifi->backoff_ms) {
                begin_join(wifi);
            }
            break;
        case WIFI_STATE_OFF:
        case WIFI_STATE_NEEDS_CONFIG:
        case WIFI_STATE_ERROR:
        default:
            break;
    }
}

wifi_state_t wifi_manager_state(const wifi_manager_t *wifi) {
    return wifi != NULL ? wifi->state : WIFI_STATE_OFF;
}

wifi_error_t wifi_manager_error(const wifi_manager_t *wifi) {
    return wifi != NULL ? wifi->error : WIFI_ERROR_NONE;
}

bool wifi_manager_is_online(const wifi_manager_t *wifi) {
    return wifi != NULL && wifi->state == WIFI_STATE_ONLINE;
}

bool wifi_manager_radio_powered(const wifi_manager_t *wifi) {
    return wifi != NULL && wifi->radio_powered;
}

uint32_t wifi_manager_backoff_ms(const wifi_manager_t *wifi) {
    return wifi != NULL ? wifi->backoff_ms : 0u;
}

const char *wifi_manager_ssid(const wifi_manager_t *wifi) {
    return wifi != NULL ? wifi->ssid : "";
}
