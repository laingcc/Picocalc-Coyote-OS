#ifndef COYOTE_CHAT_MODE_H
#define COYOTE_CHAT_MODE_H

/*
 * Keyboard-driven AI chat screen.
 *
 *   Enter      send the composer as a new turn
 *   Esc        cancel the reply in flight; otherwise clear the composer
 *   Up/Down    scroll the transcript
 *   F5         chat menu: model, provider, settings, new chat
 *
 * Nothing here blocks or draws from a callback.  Replies arrive through
 * app_services_poll; the provider callback only updates the transcript and
 * marks it dirty, and the screen is repainted from chat_mode_handle_input,
 * which main calls on every loop iteration while this mode is in front.
 */

/* Call once at startup, after app_services_init. */
void chat_mode_init(void);
void chat_mode_redraw(void);
/* c is the key read this iteration, or a negative value for none. */
void chat_mode_handle_input(int c);
/* Open the chat menu's Connection settings from any mode.  It draws over the
 * chat screen and returns once the menu is applied or cancelled; the caller
 * then redraws whichever mode is in front. */
void chat_mode_show_connection_settings(void);

#endif
