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

typedef enum { MODE_CALCULATOR, MODE_TEXT, MODE_CHAT } app_mode_t;

/* ui_show_input_dialog flags.  With none set it is the file-name prompt:
 * path characters are refused and an empty entry cannot be confirmed. */
#define UI_INPUT_ANY_CHAR    1u /* accept every printable character */
#define UI_INPUT_MASKED      2u /* echo asterisks instead of the text */
#define UI_INPUT_ALLOW_EMPTY 4u /* Enter confirms an empty entry */
/* Longest entry the dialog accepts, whatever max_len is. */
#define UI_INPUT_MAX 127
/* Width, in characters, of ui_show_list_menu; labels are cut to fit it. */
#define UI_LIST_MENU_W 34

/* Called repeatedly while a modal menu or prompt waits for input, so
 * background work (networking) keeps running.  Must be cheap and non-blocking
 * and must not open another menu. */
typedef void (*ui_idle_hook_t)(void);

void ui_init();
void ui_set_idle_hook(ui_idle_hook_t hook);
void update_active_tab(int new_tab);
TabContext* ui_get_tab_context(int tab_idx);
int ui_get_active_tab_idx();
void ui_add_to_history(int tab_idx, const char* expression, double result);
void ui_redraw_tab_content();
void ui_redraw_input_only();
void ui_show_menu();
void ui_show_mode_menu();
void ui_show_graph_menu();
bool ui_show_file_menu(const char* directory, char* out_filename, int max_len);
bool ui_show_save_prompt(char* out_filename, int max_len);
/* Modal text prompt; false if cancelled, leaving out untouched. */
bool ui_show_input_dialog(const char* title, char* out, int max_len, unsigned flags);
/* Modal pick list of up to 16 labels; returns the chosen index or -1.  The
 * caller redraws whatever the menu covered. */
int ui_show_list_menu(const char* title, const char* const* labels, int count, int sel);
app_mode_t ui_get_current_mode();
void ui_set_current_mode(app_mode_t mode);
bool ui_graph_add_function(const char* expression);
void ui_graph_clear_all();

#endif
