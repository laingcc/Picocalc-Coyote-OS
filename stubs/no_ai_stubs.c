/*
 * The AI stack, left out.
 *
 * With -DCOYOTE_NO_AI=ON the chat sources are not compiled at all; this file
 * is the handful of entry points the rest of the firmware still calls - the
 * main loop, the menus and the USB disk mode - so that none of those need to
 * know which build they are in.  Nothing here does anything, because there is
 * no chat to draw, no services to poll and no card settings to reload.
 */

#include "UI/chat_mode.h"
#include "UI/ui.h"
#include "ai/app_services.h"
#include "time/time_service.h"

void chat_mode_init(void) {}

void chat_mode_reload_settings(void) {}

void chat_mode_redraw(void) {}

void chat_mode_handle_input(int c) {
    (void)c;
}

void chat_mode_show_connection_settings(void) {
    ui_show_notice(" No AI in this build ");
}

void app_services_init(void) {}

void app_services_poll(void) {}

bool app_services_chat_active(void) {
    return false;
}

void time_service_init(const char *dir) {
    (void)dir;
}

void time_service_poll(void) {}
