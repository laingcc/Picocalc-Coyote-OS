#include "ai/app_services.h"

#include <stdint.h>
#include <string.h>

#include "pico/time.h"

#include "time/time_service.h"

#ifndef COYOTE_HAS_WIFI
#define COYOTE_HAS_WIFI 0
#endif

#if COYOTE_HAS_WIFI
#include "pico/cyw43_arch.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/altcp_tls_mbedtls_opts.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "mbedtls/ssl.h"
#include "tls/tls_trust.h"

/* lwIP's TLS layer takes its verification mode from this build option, and
 * defaults it to "optional", which would complete a handshake with a peer
 * whose certificate failed.  Refuse to build that way. */
_Static_assert(ALTCP_MBEDTLS_AUTHMODE == MBEDTLS_SSL_VERIFY_REQUIRED,
               "TLS peer verification must be mandatory");
#endif

/* All service state is static: these objects are far too large for a stack. */
static struct {
    bool initialised;
    bool polling;
    ai_config_t config;
    wifi_manager_t wifi;
    wifi_scan_t scan;
    http_stream_t stream;
    ollama_provider_t provider;
    /* The cloud providers' request and reply parser.  One serves all of
     * them: only one request is ever in flight. */
    sse_provider_t cloud;
    char cloud_path[CLOUD_PROVIDER_PATH_CAPACITY];
    provider_event_callback_t callback;
    void *callback_context;
} services;

static uint32_t platform_clock_ms(void *context) {
    (void)context;
    return to_ms_since_boot(get_absolute_time());
}

static bool platform_link_up(void *context) {
    (void)context;
    return wifi_manager_is_online(&services.wifi);
}

#if COYOTE_HAS_WIFI

/* ---- CYW43 station adapter --------------------------------------------- */

static bool radio_initialised;

static int radio_on(void *context) {
    (void)context;
    if (!radio_initialised) {
        if (cyw43_arch_init() != 0) {
            return -1;
        }
        radio_initialised = true;
    }
    cyw43_arch_enable_sta_mode();
    return 0;
}

static void radio_off(void *context) {
    (void)context;
    cyw43_arch_disable_sta_mode();
}

static int radio_join(void *context, const char *ssid, const char *password) {
    uint32_t auth = password[0] != '\0' ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    (void)context;
    return cyw43_arch_wifi_connect_async(ssid, password, auth) == 0 ? 0 : -1;
}

static void radio_leave(void *context) {
    (void)context;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}

static wifi_link_t radio_link_status(void *context) {
    (void)context;
    switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA)) {
        case CYW43_LINK_UP:
            return WIFI_LINK_UP;
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            return WIFI_LINK_JOINING;
        case CYW43_LINK_BADAUTH:
            return WIFI_LINK_BAD_AUTH;
        case CYW43_LINK_NONET:
            return WIFI_LINK_NO_NETWORK;
        case CYW43_LINK_FAIL:
            return WIFI_LINK_FAILED;
        case CYW43_LINK_DOWN:
        default:
            return WIFI_LINK_DOWN;
    }
}

/* ---- CYW43 scan adapter ------------------------------------------------ */

/*
 * A scan needs the radio up, which the station only does once it has an SSID,
 * so the scan powers it when it has to.  scan_radio is set from the start of
 * a scan until the driver has finished with it; scan_settle then hands the
 * radio back, powering it down again unless the station took it meanwhile.
 */
static bool scan_radio;

static int scan_on_result(void *env, const cyw43_ev_scan_result_t *result) {
    char ssid[WIFI_SCAN_SSID_CAPACITY];
    size_t length;
    (void)env;
    if (result == NULL) {
        return 0;
    }
    /* The driver's SSID is a counted byte string, not a C string. */
    length = result->ssid_len < sizeof(result->ssid) ? result->ssid_len : sizeof(result->ssid);
    memcpy(ssid, result->ssid, length);
    ssid[length] = '\0';
    wifi_scan_on_result(&services.scan, ssid, result->rssi);
    return 0;
}

static int scan_begin(void *context) {
    static cyw43_wifi_scan_options_t options;
    (void)context;
    if (!wifi_manager_radio_powered(&services.wifi) && radio_on(NULL) != 0) {
        return -1;
    }
    scan_radio = true;
    /* Also fails while the driver is still finishing an abandoned scan. */
    if (cyw43_wifi_scan_active(&cyw43_state) ||
        cyw43_wifi_scan(&cyw43_state, &options, NULL, scan_on_result) != 0) {
        return -1;
    }
    return 0;
}

static void scan_settle(void) {
    if (!scan_radio || cyw43_wifi_scan_active(&cyw43_state)) {
        return;
    }
    /* The driver reports completion only through its scan-active flag. */
    wifi_scan_on_done(&services.scan);
    scan_radio = false;
    if (!wifi_manager_radio_powered(&services.wifi)) {
        radio_off(NULL);
    }
}

/* ---- lwIP DNS and altcp (TCP or TLS) adapter ----------------------------- */

/*
 * One connection at a time.  net.pcb is non-NULL exactly while lwIP holds a
 * PCB whose callbacks point at us; it is cleared before the PCB is closed or
 * aborted and by the error callback, where lwIP has already freed it (or, for
 * a failed TLS handshake, frees it as soon as the callback returns).
 *
 * lwIP requires a callback that aborted its own PCB to return ERR_ABRT, so
 * each callback clears net.aborted on entry and reports it on exit.
 *
 * The PCB is an altcp one so that a plain and a TLS connection are driven by
 * the same callbacks.  Which it is, is decided only by which adapter the
 * stream called: net_tcp_connect makes a plain PCB, net_tls_connect a TLS
 * one, and neither ever falls back to the other.
 *
 * With pico_cyw43_arch_lwip_poll every lwIP callback runs inside
 * cyw43_arch_poll on the main loop, the same context that calls into lwIP
 * from here, so no locking is needed.
 */
static struct {
    struct altcp_pcb *pcb;
    ip_addr_t address;
    uintptr_t dns_generation;
    bool aborted;
    bool tls; /* pcb is a TLS connection */
    /* Trust anchors and RNG for TLS, held from the first TLS connect until
     * net_tls_reap finds the transport idle.  mbedTLS allocates these, like
     * everything else it needs, from lwIP's fixed heap (MEM_SIZE), never the
     * C heap. */
    struct altcp_tls_config *tls_config;
} net;

static void net_detach(struct altcp_pcb *pcb) {
    altcp_arg(pcb, NULL);
    altcp_recv(pcb, NULL);
    altcp_sent(pcb, NULL);
    altcp_err(pcb, NULL);
}

static void net_tcp_abort(void *context) {
    struct altcp_pcb *pcb = net.pcb;
    (void)context;
    if (pcb == NULL) {
        return;
    }
    net.pcb = NULL;
    net.aborted = true;
    net_detach(pcb);
    altcp_abort(pcb);
}

static void net_tcp_close(void *context) {
    struct altcp_pcb *pcb = net.pcb;
    (void)context;
    if (pcb == NULL) {
        return;
    }
    net.pcb = NULL;
    net_detach(pcb);
    if (altcp_close(pcb) != ERR_OK) {
        /* No memory for the FIN: drop the connection rather than leak it. */
        net.aborted = true;
        altcp_abort(pcb);
    }
}

static err_t net_callback_result(void) {
    return net.aborted ? ERR_ABRT : ERR_OK;
}

/* The handshake is over and the certificate chain and host name verified.
 * With verification mandatory a handshake cannot complete otherwise; this is
 * checked again here so that no request byte depends on that alone. */
static bool net_tls_verified(struct altcp_pcb *pcb) {
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    return ssl != NULL && mbedtls_ssl_is_handshake_over(ssl) && mbedtls_ssl_get_verify_result(ssl) == 0u;
}

/* For a TLS connection lwIP calls this only once the handshake has finished. */
static err_t net_on_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    bool connected = err == ERR_OK;
    (void)arg;
    net.aborted = false;
    if (connected && net.tls) {
        connected = net_tls_verified(pcb);
    }
    /* On failure the stream aborts the PCB through net_tcp_abort. */
    http_stream_on_connected(&services.stream, connected);
    return net_callback_result();
}

static err_t net_on_sent(void *arg, struct altcp_pcb *pcb, u16_t length) {
    (void)arg;
    (void)pcb;
    net.aborted = false;
    http_stream_on_sent(&services.stream, length);
    return net_callback_result();
}

static err_t net_on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct pbuf *q;
    (void)arg;
    net.aborted = false;

    if (p == NULL) {
        /* The peer closed.  The stream settles and releases the PCB. */
        http_stream_on_closed(&services.stream);
        if (net.pcb == pcb) {
            net_tcp_close(NULL);
        }
        return net_callback_result();
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return ERR_OK;
    }

    /* Hand each segment straight to the stream; stop as soon as it lets go of
     * the connection (done, failed or cancelled mid-chain). */
    for (q = p; q != NULL && net.pcb == pcb; q = q->next) {
        http_stream_on_recv(&services.stream, (const char *)q->payload, q->len);
    }
    if (net.pcb == pcb) {
        altcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return net_callback_result();
}

/* The connection is gone: reset, refused, out of memory, or a TLS handshake
 * that failed (bad certificate, wrong host, no usable clock).  lwIP frees the
 * PCB itself, so it is only forgotten here, never closed. */
static void net_on_error(void *arg, err_t err) {
    (void)arg;
    (void)err;
    net.pcb = NULL;
    http_stream_on_error(&services.stream);
}

/* lwIP cannot cancel a lookup, so each one carries a generation number and an
 * answer for an abandoned lookup is dropped. */
static void net_on_dns(const char *name, const ip_addr_t *address, void *arg) {
    (void)name;
    if ((uintptr_t)arg != net.dns_generation) {
        return;
    }
    if (address != NULL) {
        net.address = *address;
    }
    http_stream_on_dns(&services.stream, address != NULL);
}

static int net_dns_resolve(void *context, const char *host) {
    err_t err;
    (void)context;
    net.dns_generation++;
    err = dns_gethostbyname(host, &net.address, net_on_dns, (void *)net.dns_generation);
    if (err == ERR_OK) {
        return HTTP_STREAM_IO_DONE;
    }
    return err == ERR_INPROGRESS ? HTTP_STREAM_IO_PENDING : HTTP_STREAM_IO_ERROR;
}

/* Attach the callbacks and start connecting; on failure the PCB is freed. */
static int net_connect(struct altcp_pcb *pcb, uint16_t port, bool tls) {
    altcp_arg(pcb, NULL);
    altcp_recv(pcb, net_on_recv);
    altcp_sent(pcb, net_on_sent);
    altcp_err(pcb, net_on_error);
    if (altcp_connect(pcb, &net.address, port, net_on_connected) != ERR_OK) {
        net_detach(pcb);
        altcp_abort(pcb);
        return HTTP_STREAM_IO_ERROR;
    }
    net.pcb = pcb;
    net.tls = tls;
    return HTTP_STREAM_IO_PENDING;
}

static int net_tcp_connect(void *context, uint16_t port) {
    struct altcp_pcb *pcb;
    (void)context;
    if (net.pcb != NULL) {
        return HTTP_STREAM_IO_ERROR;
    }
    pcb = altcp_tcp_new_ip_type(IP_GET_TYPE(&net.address));
    if (pcb == NULL) {
        return HTTP_STREAM_IO_ERROR;
    }
    return net_connect(pcb, port, false);
}

/*
 * Open a TLS connection that verifies the peer against the embedded roots
 * (tls_trust.c) and against host.  Every step is required: if the trust
 * anchors cannot be loaded, the PCB cannot be made, or the host name cannot be
 * set, no connection is attempted at all.
 *
 * The certificate dates are checked against the clock mbedTLS is built to use
 * (tls/tls_clock.c), which only knows the time once SNTP has set it.
 */
static int net_tls_connect(void *context, const char *host, uint16_t port) {
    struct altcp_pcb *pcb;
    mbedtls_ssl_context *ssl;
    (void)context;

    if (net.pcb != NULL || host == NULL || host[0] == '\0') {
        return HTTP_STREAM_IO_ERROR;
    }
    if (net.tls_config == NULL) {
        net.tls_config = altcp_tls_create_config_client(tls_trust_pem, tls_trust_pem_length);
        if (net.tls_config == NULL) {
            return HTTP_STREAM_IO_ERROR;
        }
    }
    pcb = altcp_tls_new(net.tls_config, IP_GET_TYPE(&net.address));
    if (pcb == NULL) {
        return HTTP_STREAM_IO_ERROR;
    }
    /* The host name is both the SNI sent to the server and the name its
     * certificate must carry; without it mbedTLS would accept a valid
     * certificate for any host.  The authentication mode is already REQUIRED
     * (see the assertion above) and is set again on this connection's
     * configuration so that it does not rest on a build option alone. */
    ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (ssl == NULL || ssl->MBEDTLS_PRIVATE(conf) == NULL || mbedtls_ssl_set_hostname(ssl, host) != 0) {
        altcp_abort(pcb);
        return HTTP_STREAM_IO_ERROR;
    }
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)ssl->MBEDTLS_PRIVATE(conf), MBEDTLS_SSL_VERIFY_REQUIRED);
    return net_connect(pcb, port, true);
}

/* Free the TLS configuration once nothing can still be using it.  This runs
 * from the main loop, never from an lwIP callback: after a failed handshake
 * lwIP is still tearing the connection down when it reports the error. */
static void net_tls_reap(void) {
    if (net.tls_config != NULL && net.pcb == NULL && !http_stream_is_active(&services.stream)) {
        altcp_tls_free_config(net.tls_config);
        net.tls_config = NULL;
    }
}

static int net_tcp_write(void *context, const char *data, size_t length) {
    size_t room;
    err_t err;
    (void)context;
    if (net.pcb == NULL) {
        return HTTP_STREAM_IO_ERROR;
    }
    room = altcp_sndbuf(net.pcb);
    if (length > room) {
        length = room;
    }
    if (length == 0u) {
        return 0;
    }
    err = altcp_write(net.pcb, data, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) {
        return 0; /* no segment memory yet: retried after an ack or on poll */
    }
    if (err != ERR_OK) {
        return HTTP_STREAM_IO_ERROR;
    }
    altcp_output(net.pcb);
    return (int)length;
}

#define NET_TLS_CONNECT net_tls_connect

static void platform_poll(void) {
    if (wifi_manager_radio_powered(&services.wifi) || scan_radio) {
        cyw43_arch_poll();
    }
}

#else /* !COYOTE_HAS_WIFI */

/* No radio on this board: the station can never come up, so the stream's
 * link check keeps every request from reaching the adapters below. */

static int radio_on(void *context) {
    (void)context;
    return -1;
}

static void radio_off(void *context) {
    (void)context;
}

static int radio_join(void *context, const char *ssid, const char *password) {
    (void)context;
    (void)ssid;
    (void)password;
    return -1;
}

static void radio_leave(void *context) {
    (void)context;
}

static wifi_link_t radio_link_status(void *context) {
    (void)context;
    return WIFI_LINK_DOWN;
}

static int scan_begin(void *context) {
    (void)context;
    return -1;
}

static void scan_settle(void) {
}

static int net_dns_resolve(void *context, const char *host) {
    (void)context;
    (void)host;
    return HTTP_STREAM_IO_ERROR;
}

static int net_tcp_connect(void *context, uint16_t port) {
    (void)context;
    (void)port;
    return HTTP_STREAM_IO_ERROR;
}

static int net_tcp_write(void *context, const char *data, size_t length) {
    (void)context;
    (void)data;
    (void)length;
    return HTTP_STREAM_IO_ERROR;
}

static void net_tcp_close(void *context) {
    (void)context;
}

static void net_tcp_abort(void *context) {
    (void)context;
}

/* No TLS adapter: the stream refuses to start a TLS request without one. */
#define NET_TLS_CONNECT NULL

static void net_tls_reap(void) {
}

static void platform_poll(void) {
}

#endif /* COYOTE_HAS_WIFI */

static const wifi_manager_ops_t wifi_ops = {
    platform_clock_ms, radio_on, radio_off, radio_join, radio_leave, radio_link_status,
};

static const wifi_scan_ops_t scan_ops = {
    platform_clock_ms, scan_begin,
};

static const http_stream_ops_t stream_ops = {
    platform_clock_ms, platform_link_up, net_dns_resolve, net_tcp_connect,
    net_tcp_write, net_tcp_close, net_tcp_abort, NET_TLS_CONNECT,
};

/* ---- Provider binding --------------------------------------------------- */

static int sink_feed(void *context, const char *data, size_t length) {
    return ollama_provider_feed((ollama_provider_t *)context, data, length);
}

static int sink_finish(void *context) {
    return ollama_provider_finish((ollama_provider_t *)context);
}

/* A cloud provider answers an error status (a bad key, no balance, rate
 * limiting) with a body that is not an event stream.  It is dropped unread:
 * the stream reports the status itself, and the body is never shown. */
static bool cloud_status_ok(void) {
    int status = http_stream_status_code(&services.stream);
    return status >= 200 && status <= 299;
}

static int cloud_sink_feed(void *context, const char *data, size_t length) {
    return cloud_status_ok() ? sse_provider_feed((sse_provider_t *)context, data, length) : 0;
}

static int cloud_sink_finish(void *context) {
    return cloud_status_ok() ? sse_provider_finish((sse_provider_t *)context) : 0;
}

/* One received segment can decode into several events.  Once the chat has been
 * cancelled (possibly by the callback itself) the rest are not forwarded. */
static void on_provider_event(void *context, const provider_event_t *event) {
    (void)context;
    if (services.callback == NULL || !http_stream_is_active(&services.stream)) {
        return;
    }
    services.callback(services.callback_context, event);
}

/* ---- Public interface --------------------------------------------------- */

void app_services_init(void) {
    memset(&services, 0, sizeof(services));
    ai_config_init(&services.config);
    wifi_manager_init(&services.wifi, &wifi_ops, NULL);
    wifi_scan_init(&services.scan, &scan_ops, NULL);
    http_stream_init(&services.stream, &stream_ops, NULL);
    ollama_provider_init(&services.provider, on_provider_event, NULL);
    sse_provider_init(&services.cloud, on_provider_event, NULL);
    services.initialised = true;
}

void app_services_poll(void) {
    if (!services.initialised || services.polling) {
        return;
    }
    services.polling = true;
    platform_poll();
    wifi_manager_poll(&services.wifi);
    app_services_wifi_scan_poll();
    http_stream_poll(&services.stream);
    net_tls_reap();
    services.polling = false;
}

int app_services_configure(const ai_config_t *config) {
    if (!services.initialised || config == NULL) {
        return -1;
    }
    /* Validate first: a rejected config must not disturb an in-flight chat. */
    if (wifi_manager_set_credentials(&services.wifi, config->ssid, config->password) != 0) {
        return -1;
    }
    http_stream_cancel(&services.stream);
    services.config = *config;
    wifi_manager_enable(&services.wifi);
    return 0;
}

wifi_state_t app_services_wifi_state(void) {
    return wifi_manager_state(&services.wifi);
}

wifi_error_t app_services_wifi_error(void) {
    return wifi_manager_error(&services.wifi);
}

int app_services_wifi_scan_start(void) {
    if (!services.initialised) {
        return -1;
    }
    return wifi_scan_start(&services.scan);
}

void app_services_wifi_scan_poll(void) {
    if (!services.initialised) {
        return;
    }
    scan_settle();
    wifi_scan_poll(&services.scan);
}

wifi_scan_state_t app_services_wifi_scan_state(void) {
    return services.initialised ? wifi_scan_state(&services.scan) : WIFI_SCAN_IDLE;
}

size_t app_services_wifi_scan_count(void) {
    return services.initialised ? wifi_scan_count(&services.scan) : 0u;
}

int app_services_wifi_scan_at(size_t i, char *ssid_buf, int *rssi) {
    const char *ssid = services.initialised ? wifi_scan_ssid(&services.scan, i) : NULL;
    if (ssid == NULL) {
        return -1;
    }
    if (ssid_buf != NULL) {
        memcpy(ssid_buf, ssid, strlen(ssid) + 1u);
    }
    if (rssi != NULL) {
        *rssi = wifi_scan_rssi(&services.scan, i);
    }
    return 0;
}

cloud_start_t app_services_cloud_ready(void) {
    uint32_t now;
    return cloud_provider_start_check(&services.config, services.initialised && time_service_trusted_now(&now));
}

deepseek_start_t app_services_deepseek_ready(void) {
    return ai_config_is_deepseek(&services.config) ? app_services_cloud_ready() : DEEPSEEK_START_NOT_DEEPSEEK;
}

/*
 * A cloud request: always TLS, always to the provider's own fixed endpoint
 * (never to the configured host, which the start check has in any case
 * confirmed is the same), with the API key in that provider's header.
 * cloud_provider_prepare puts it together; it is refused before any network
 * activity unless the configuration is complete and the clock has been set by
 * SNTP since boot.
 */
static int chat_start_cloud(const ollama_message_t *messages, size_t message_count) {
    http_stream_request_t request;
    uint32_t now;

    sse_provider_init(&services.cloud, on_provider_event, NULL);
    if (cloud_provider_prepare(&services.config, time_service_trusted_now(&now), messages, message_count,
                               &services.cloud, services.cloud_path, &request, NULL) != CLOUD_PREPARE_OK) {
        return -1;
    }
    request.sink.feed = cloud_sink_feed;
    request.sink.finish = cloud_sink_finish;
    request.sink.context = &services.cloud;
    request.connect_timeout_ms = services.config.connect_timeout_ms;
    request.idle_timeout_ms = services.config.idle_timeout_ms;
    request.request_timeout_ms = services.config.request_timeout_ms;
    return http_stream_start(&services.stream, &request);
}

int app_services_chat_start(const ollama_message_t *messages,
                            size_t message_count,
                            provider_event_callback_t callback,
                            void *context) {
    http_stream_request_t request;

    if (!services.initialised || http_stream_is_active(&services.stream) ||
        !wifi_manager_is_online(&services.wifi)) {
        return -1;
    }

    services.callback = callback;
    services.callback_context = context;
    if (ai_config_cloud(&services.config) != NULL) {
        return chat_start_cloud(messages, message_count);
    }
    if (strcmp(services.config.provider, AI_CONFIG_PROVIDER_OLLAMA) != 0) {
        return -1;
    }
    ollama_provider_init(&services.provider, on_provider_event, NULL);
    if (ollama_provider_build_request(&services.provider, services.config.model, messages, message_count,
                                      (int)services.config.max_predict,
                                      services.config.system_prompt,
                                      (int)services.config.temperature) != 0) {
        return -1;
    }

    memset(&request, 0, sizeof(request));
    request.host = services.config.host;
    request.port = services.config.port;
    request.method = ollama_provider_method();
    request.path = ollama_provider_path();
    request.content_type = ollama_provider_content_type();
    request.body = ollama_provider_request(&services.provider);
    request.sink.feed = sink_feed;
    request.sink.finish = sink_finish;
    request.sink.context = &services.provider;
    request.connect_timeout_ms = services.config.connect_timeout_ms;
    request.idle_timeout_ms = services.config.idle_timeout_ms;
    request.request_timeout_ms = services.config.request_timeout_ms;
    return http_stream_start(&services.stream, &request);
}

void app_services_chat_cancel(void) {
    http_stream_cancel(&services.stream);
}

bool app_services_chat_active(void) {
    return http_stream_is_active(&services.stream);
}

http_stream_state_t app_services_chat_state(void) {
    return http_stream_state(&services.stream);
}

http_stream_error_t app_services_chat_error(void) {
    return http_stream_error(&services.stream);
}

int app_services_chat_status_code(void) {
    return http_stream_status_code(&services.stream);
}
