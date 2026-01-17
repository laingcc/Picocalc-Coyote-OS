//
// Created by chuck on 2025-12-19.
//

#ifndef COYOTE_UI_H
#define COYOTE_UI_H

#include <stdbool.h>

#define MAX_TABS 4
#define MAX_HISTORY 10
#define INPUT_BUFFER_SIZE 128

typedef struct {
    char expression[INPUT_BUFFER_SIZE];
    double result;
    bool has_result;
} HistoryItem;

typedef struct {
    HistoryItem history[MAX_HISTORY];
    int history_count;
    char current_input[INPUT_BUFFER_SIZE];
    int input_index;
} TabContext;

void ui_init();

void update_tab_count(int new_count);
void update_active_tab(int new_tab);
void reboot_to_bootloader();

TabContext* ui_get_tab_context(int tab_idx);
int ui_get_active_tab_idx();
void ui_add_to_history(int tab_idx, const char* expression, double result);
void ui_redraw_tab_content();
void ui_redraw_input_only();

#endif //COYOTE_UI_H