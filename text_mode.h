#ifndef TEXT_MODE_H
#define TEXT_MODE_H

void text_mode_init();
void text_mode_handle_input(int c);
void text_mode_redraw();

// Buffer access for save/load operations
const char* text_mode_get_buffer();
void text_mode_set_buffer(const char* content);

#endif // TEXT_MODE_H
