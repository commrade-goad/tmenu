#include <stdio.h>
#include <unistd.h>
#include <curses.h>
#include "helpa.h"
#include "config.h"

// TODO: custom key, custom color, exec mode, prompt, C-a, C-e text manipulation

typedef struct {
    HStrView *dt;
    size_t    cp;
    size_t    sz;
} Entry;

static Entry entry = {0};
static size_t selected = 0;
static size_t scroll_offset = 0;

#define CTRL(c) ((c) & 0x1F)

bool hstrview_contain(HStrView *str, HStrView *cont) {
    if (cont->sz == 0) return true;
    if (cont->sz > str->sz) return false;

    for (size_t i = 0; i <= str->sz - cont->sz; i++) {
        bool match = true;
        for (size_t j = 0; j < cont->sz; j++) {
            if (str->dt[i + j] != cont->dt[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void reset_selected(Entry *current_display) {
    if (current_display->sz > 0 && current_display->sz <= selected) { selected = current_display->sz - 1; }
    if (current_display->sz > 0 && current_display->sz <= scroll_offset) { scroll_offset = current_display->sz - 1; }
}

void split_entry(HStr *str, char split) {
    u8 *before = str->dt;
    helpa_da_foreach(*str, c) {
        if (*c == split) {
            HStrView strv = {
                .dt = before,
                .sz = (i32) (c - before),
            };
            before = c + 1;
            helpa_da_append(entry, strv);
        }
    }
    if (before < str->dt + str->sz) {
        HStrView strv = {
            .dt = before,
            .sz = (i32)((str->dt + str->sz) - before),
        };
        if (strv.sz <= 0 ||
            (strv.sz == 1 && isspace(*strv.dt))) {
            return;
        }
        helpa_da_append(entry, strv);
    }
    return;
}

int main(int argc, char **argv) {
    HStr buffer = {
        .dt = NULL,
        .cp = 1024,
        .sz = 0,
    };
    buffer.dt = malloc(buffer.cp);

    ssize_t n ;
    do {
        n = read(0, buffer.dt + buffer.sz, buffer.cp - buffer.sz);
        buffer.sz += n;
        if (buffer.sz == buffer.cp) {
            buffer.cp *= 2;
            buffer.dt = realloc(buffer.dt, buffer.cp);
        }
    } while (n > 0);

    split_entry(&buffer, '\n');

    // init ncurses stuff
    SCREEN *scr = NULL;
    if (!isatty(0))  {
        FILE *tty_in  = fopen("/dev/tty", "r");
        FILE *tty_out = fopen("/dev/tty", "w");

        if (!tty_in || !tty_out) {
            fprintf(stderr, "failed to open /dev/tty\n");
            return 1;
        }

        scr = newterm(NULL, tty_out, tty_in);
        set_term(scr);
    } else {
        initscr();
    }

    // proper setup
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(25);
    set_escdelay(25);
    use_default_colors();
    // nodelay(stdscr, TRUE);

    size_t mx = getmaxx(stdscr);
    size_t my = getmaxy(stdscr);

    bool running = true;
    HStr user_buffer = {0};
    mvcur(0, 4, 0, 4);

    Entry search_results = {0};      // Dedicated list for filtered items
    Entry *current_display = &entry; // Point to original list by default

    init_pair(1, -1, SELECT_COLOR);

    bool dont_echo = false;

    while (running) {
        i32 ch = getch();
        switch (ch) {
        case KEY_RESIZE: {
            mx = getmaxx(stdscr);
            my = getmaxy(stdscr);
        } break;
        case KEY_BACKSPACE: {
            if (user_buffer.sz > 0) {
                hstr_pop(&user_buffer);
                // selected = 0;
                // scroll_offset = 0;
                reset_selected(current_display);
            }
        } break;
        case CTRL('n'): { // Ctrl + N (Move Down)
            if (current_display->sz > 0 && selected < current_display->sz - 1) {
                selected++;
            }
        } break;

        case CTRL('p'): { // Ctrl + P (Move Up)
            if (selected > 0) {
                selected--;
            }
        } break;

        case CTRL('g'):
        case 27: { // this is escape key
            dont_echo = true;
            running = false;
        } break;

        default: {
            if (((char)ch >= '!' && (char)ch <= '~') || (char)ch == ' ') {
                hstr_push(&user_buffer, (char)ch);
                // selected      = 0;
                // scroll_offset = 0;
                reset_selected(current_display);
            }
            if ((char)ch == '\n') running = false;
        } break;
        }

        size_t input_len = user_buffer.sz + strlen(PROMPT);
        size_t input_rows = (input_len + mx - 1) / mx;
        size_t i = input_rows;

        size_t max_visible_items = (my > input_rows) ? (my - input_rows) : 0;
        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + max_visible_items) {
            scroll_offset = selected - max_visible_items + 1;
        }

        if (user_buffer.sz > 0) {
            search_results.sz = 0;
            HStrView converted = {
                .dt = user_buffer.dt,
                .sz = user_buffer.sz,
            };

            helpa_da_foreach(entry, items) {
                if (hstrview_contain(items, &converted)) {
                    helpa_da_append(search_results, *items);
                }
            }

            current_display = &search_results;
            wclear(stdscr);
        } else {
            current_display = &entry;
            wclear(stdscr);
        }

        reset_selected(current_display);

        size_t index = 0;
        helpa_da_foreach(*current_display, items) {
            if (index >= scroll_offset && index < scroll_offset + max_visible_items) {
                i32 len = (i32)items->sz;
                if (items->sz > mx) len = (i32)mx;

                mvprintw(i, 0, "%.*s\n", len, items->dt);
                if (index == selected) {
                    mvchgat(i, 0, -1, A_NORMAL, 1, NULL);
                }
                i++;
            }
            index++;
        }
        mvprintw(0, 0, "%s%.*s", PROMPT, (i32)user_buffer.sz, user_buffer.dt);

        wclrtoeol(stdscr);
        refresh();
    }

    endwin();

    if (dont_echo) return current_display->sz > 0 ? 0 : 1;
    if (current_display->sz > 0) {
        HStrView selected_entry = current_display->dt[selected];
        printf("%.*s\n", (i32)selected_entry.sz, selected_entry.dt);
        return 0;
    }
    printf("%.*s\n", (i32)user_buffer.sz, user_buffer.dt);
    return 1;
}
