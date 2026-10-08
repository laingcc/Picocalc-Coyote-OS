#ifndef COYOTE_AI_APP_SERVICES_H
#define COYOTE_AI_APP_SERVICES_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/ai_config.h"
#include "ai/deepseek_provider.h"
#include "ai/http_stream.h"
#include "ai/ollama_provider.h"
#include "ai/provider.h"
#include "ai/wifi_manager.h"
#include "ai/wifi_scan.h"

/*
 * Firmware binding for the AI chat path.
 *
 * app_services owns the single static instances of the configuration, the
 * Wi-Fi manager, the network scan, the HTTP stream and the Ollama provider,
 * and wires the state machines to CYW43 and lwIP raw callbacks
 * (pico_cyw43_arch_lwip_poll).
 * Everything runs from app_services_poll on the main loop; there are no
 * threads, no blocking network calls and no heap allocations here.
 *
 * Until app_services_configure supplies an SSID or a network scan is asked
 * for the radio is never powered, so a unit without configuration behaves
 * exactly as it did before.
 *
 * The stored configuration holds the Wi-Fi password.  No function in this
 * interface returns it or the configuration that contains it, and nothing
 * here logs.
 *
 * When the board has no CYW43 radio the same interface is built without
 * networking: the Wi-Fi state reports ERROR, a scan fails to start and no
 * chat request can start.
 */

/* Reset all service state.  Does not touch the radio. */
void app_services_init(void);

/*
 * Service the radio, lwIP timers, the Wi-Fi state machine, the network scan
 * and request timeouts.  Cheap and non-blocking; call it on every main loop iteration and
 * from any loop that waits for input.  Calls made while a poll is already
 * running (for example from a callback that opens a menu) return immediately.
 */
void app_services_poll(void);

/*
 * Adopt a configuration: cancels any chat in flight, stores a copy, and
 * (re)starts the Wi-Fi station with its credentials.  Returns -1 if config is
 * NULL or its credentials do not fit.
 */
int app_services_configure(const ai_config_t *config);

wifi_state_t app_services_wifi_state(void);
wifi_error_t app_services_wifi_error(void);

/*
 * Begin scanning for Wi-Fi networks, powering the radio if the station has
 * not.  Returns 0 once the scan is under way; progress is read from
 * app_services_wifi_scan_state.  Returns -1 if a scan is already running or
 * the radio cannot scan, which (unless one was already running) leaves the
 * state at ERROR.
 */
int app_services_wifi_scan_start(void);

/* Advance the scan: completion, timeout and radio power.  Called from
 * app_services_poll; cheap and non-blocking. */
void app_services_wifi_scan_poll(void);

wifi_scan_state_t app_services_wifi_scan_state(void);
/* Networks found, strongest first; at most WIFI_SCAN_MAX_NETWORKS. */
size_t app_services_wifi_scan_count(void);
/*
 * Copy result i: its SSID into ssid_buf, which must hold
 * WIFI_SCAN_SSID_CAPACITY bytes, and its signal strength in dBm into *rssi.
 * Either may be NULL.  Returns -1 if i is out of range.
 */
int app_services_wifi_scan_at(size_t i, char *ssid_buf, int *rssi);

/*
 * Send messages to the configured Ollama host and stream the reply to
 * callback as provider events.  Events are delivered from inside
 * app_services_poll and stop as soon as the chat is cancelled.  The message
 * strings are serialised before this returns and need not outlive the call.
 *
 * Returns 0 once the request is under way.  Returns -1 if Wi-Fi is not
 * online, a chat is already active, the provider is not "ollama", or the
 * request cannot be built (no model, too many or too large messages).
 * Progress and the final outcome are read from app_services_chat_state and
 * app_services_chat_error.
 */
int app_services_chat_start(const ollama_message_t *messages,
                            size_t message_count,
                            provider_event_callback_t callback,
                            void *context);

/* Abandon the chat in flight, if any.  Safe from inside the event callback. */
void app_services_chat_cancel(void);

bool app_services_chat_active(void);
http_stream_state_t app_services_chat_state(void);
http_stream_error_t app_services_chat_error(void);
/* HTTP status of the current or last response; 0 if none was received. */
int app_services_chat_status_code(void);

#endif
