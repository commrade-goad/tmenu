#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <curses.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <wchar.h>
#include <wctype.h>
#include "helpa.h"
#include "config.h"

// TODO: exec mode, flag, regex

#define CCTRL(c) ((c) & 0x1E)

typedef struct {
    HStrView entry;
    i32      score;
} Match;

typedef struct {
    Match  *dt;
    size_t  cp;
    size_t  sz;
} Entry;

static Entry entry          = {0};
static size_t selected      = 0;
static size_t scroll_offset = 0;

static bool rl_input_avail_flag = false;
static u8   rl_input_char       = 0;
static bool running             = true;
static bool dont_echo           = false;

static size_t mx = 80, my = 24;

static int rl_input_avail(void) { return rl_input_avail_flag; }

static int rl_getc_cb(FILE *dummy) {
    (void)dummy;
    rl_input_avail_flag = false;
    return rl_input_char;
}

static void forward_to_readline(int c) {
    rl_input_char       = (unsigned char)c;
    rl_input_avail_flag = true;
    rl_callback_read_char();
}

static size_t strnwidth(const char *s, size_t n, size_t offset) {
    mbstate_t st;
    wchar_t wc;
    size_t wc_len, width = 0;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < n; i += wc_len) {
        wc_len = mbrtowc(&wc, s + i, MB_CUR_MAX, &st);
        if (wc_len == 0) break;
        if (wc_len == (size_t)-1 || wc_len == (size_t)-2) {
            width += strnlen(s + i, n - i);
            break;
        }
        if (wc == '\t')
            width = ((width + offset + 8) & ~7) - offset;
        else
            width += iswcntrl(wc) ? 2 : (size_t)(wcwidth(wc) > 0 ? wcwidth(wc) : 0);
    }
    return width;
}

bool hstrview_contain(HStrView *str, HStrView *cont) {
    if (cont->sz == 0) return true;
    if (cont->sz > str->sz) return false;
    for (size_t i = 0; i <= str->sz - cont->sz; i++) {
        bool match = true;
        for (size_t j = 0; j < cont->sz; j++) {
            if (str->dt[i+j] != cont->dt[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

int hstrview_score(HStrView *str, HStrView *cont) {
    if (cont->sz == 0) return 0;
    if (cont->sz > str->sz) return 9999;
    int best = 9999;

    for (size_t i = 0; i <= str->sz - cont->sz; i++) {
        int score = 0;
        bool match = true;
        for (size_t j = 0; j < cont->sz; j++) {
            if (str->dt[i+j] != cont->dt[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            score = (int)i;
            score += (int)(str->sz - cont->sz);
            if (score < best) best = score;
        }
    }
    return best;
}

void reset_selected(Entry *cur) {
    if (cur->sz > 0 && cur->sz <= selected)      selected     = cur->sz - 1;
    if (cur->sz > 0 && cur->sz <= scroll_offset) scroll_offset = cur->sz - 1;
}

void split_entry(HStr *str, char split) {
    u8 *before = str->dt;
    helpa_da_foreach(*str, c) {
        if (*c == split) {
            HStrView sv = { .dt = before, .sz = (i32)(c - before) };
            Match m = { .entry = sv, .score = 0 };
            before = c + 1;
            helpa_da_append(entry, m);
        }
    }
    if (before < str->dt + str->sz) {
        HStrView sv = { .dt = before, .sz = (i32)((str->dt + str->sz) - before) };
        if (sv.sz <= 0 || (sv.sz == 1 && isspace(*sv.dt))) return;
        Match m = { .entry = sv, .score = 0 };
        helpa_da_append(entry, m);
    }
}

static void redisplay(void) {
    // readline calls this whenever the line changes
    // we'll do the actual draw in the main loop, so just mark dirty
    // (alternatively draw here — but doing it in the loop keeps things simple)
}

static void line_handler(char *line) {
    if (!line) {
        // Ctrl-D on empty line → treat as cancel
        dont_echo = true;
        running   = false;
        return;
    }
    running = false;
}

// int match_cmp(const void *a, const void *b) {
//     const Match *ma = (const Match*)a;
//     const Match *mb = (const Match*)b;
//
//     // lower score = better
//     if (ma->score < mb->score) return -1;
//     if (ma->score > mb->score) return 1;
//
//     // tie-breaker (optional): shorter string first
//     if (ma->entry.sz < mb->entry.sz) return -1;
//     if (ma->entry.sz > mb->entry.sz) return 1;
//
//     return 0;
// }

int match_cmp(const void *a, const void *b) {
    const Match *ma = (const Match*)a;
    const Match *mb = (const Match*)b;

    if (ma->score != mb->score)
        return ma->score - mb->score;

    size_t min = ma->entry.sz < mb->entry.sz ? ma->entry.sz : mb->entry.sz;
    int cmp = memcmp(ma->entry.dt, mb->entry.dt, min);
    if (cmp != 0) return cmp;

    return (int)(ma->entry.sz - mb->entry.sz);
}

int main(int argc, char **argv) {
    if (!setlocale(LC_ALL, ""))
        fprintf(stderr, "warning: failed to set locale\n");

    // Read stdin
    HStr buffer = { .dt = NULL, .cp = 1024, .sz = 0 };
    buffer.dt = malloc(buffer.cp);
    ssize_t n;
    do {
        n = read(0, buffer.dt + buffer.sz, buffer.cp - buffer.sz);
        if (n > 0) {
            buffer.sz += n;
            if (buffer.sz == buffer.cp) {
                buffer.cp *= 2;
                buffer.dt = realloc(buffer.dt, buffer.cp);
            }
        }
    } while (n > 0);

    split_entry(&buffer, '\n');

    // Init ncurses
    SCREEN *scr = NULL;
    if (!isatty(0)) {
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

    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    timeout(25);
    set_escdelay(25);
    use_default_colors();

    mx = getmaxx(stdscr);
    my = getmaxy(stdscr);

    init_pair(1, -1, SELECT_COLOR);

    // Init readline (callback / alternate interface)
    // Tell readline not to mess with terminal settings — ncurses owns the terminal
    rl_catch_signals        = 0;
    rl_catch_sigwinch       = 0;
    rl_deprep_term_function = NULL;
    rl_prep_term_function   = NULL;
    rl_change_environment   = 0;

    rl_getc_function        = rl_getc_cb;
    rl_input_available_hook = rl_input_avail;
    rl_redisplay_function   = redisplay;

    rl_callback_handler_install(PROMPT, line_handler);

    Entry search_results   = {0};
    Entry *current_display = &entry;
    bool changed = true;

    while (running) {
        const char *line_buf = rl_line_buffer ? rl_line_buffer : "";
        size_t      line_sz  = strlen(line_buf);

        size_t prompt_width = strlen(PROMPT);
        size_t input_len    = prompt_width + strnwidth(line_buf, line_sz, prompt_width);
        size_t input_rows   = (input_len + mx - 1) / mx;
        if (input_rows == 0) input_rows = 1;

        size_t max_visible = (my > input_rows) ? (my - input_rows) : 0;

        if (changed) {
            if (line_sz > 0) {
                search_results.sz = 0;
                HStrView converted = { .dt = (u8*)line_buf, .sz = line_sz };
                helpa_da_foreach(entry, items) {
                    int score = hstrview_score(&items->entry, &converted);

                    if (score < 9999) {
                        Match m = { .entry = items->entry, .score = score };
                        helpa_da_append(search_results, m);
                    }
                    // if (hstrview_contain(&items->entry, &converted))
                    //     helpa_da_append(search_results, *items);
                }
                current_display = &search_results;
                qsort(search_results.dt, search_results.sz, sizeof(Match), match_cmp);
            } else {
                current_display = &entry;
            }
            changed = false;
        }

        reset_selected(current_display);

        if (selected < scroll_offset)
            scroll_offset = selected;
        else if (selected >= scroll_offset + max_visible && max_visible > 0)
            scroll_offset = selected - max_visible + 1;

        wclear(stdscr);

        size_t row = input_rows, index = 0;
        helpa_da_foreach(*current_display, items) {
            if (index >= scroll_offset && index < scroll_offset + max_visible) {
                int len = (int)items->entry.sz;
                if ((size_t)len > mx) len = (int)mx;
                mvprintw((int)row, 0, "%.*s", len, items->entry.dt);
                if (index == selected)
                    mvchgat((int)row, 0, -1, A_NORMAL, 1, NULL);
                row++;
            }
            index++;
        }

        mvprintw(0, 0, "%s%s", PROMPT, line_buf);
        wclrtoeol(stdscr);

        size_t cursor_col = prompt_width +
            strnwidth(line_buf, (size_t)rl_point, prompt_width);
        if (cursor_col < mx)
            move(0, (int)cursor_col);

        refresh();

        int ch = getch();
        if (!running) break;

        switch (ch) {
            case ERR: {} break;
            case KEY_RESIZE: {
                mx = getmaxx(stdscr);
                my = getmaxy(stdscr);
            } break;
            // C-n / C-p: navigate list (intercept before readline sees them)
            case CCTRL('n'): {
                if (current_display->sz > 0 && selected < current_display->sz - 1)
                    selected++;
            } break;

            case CCTRL('p'): {
                if (selected > 0)
                    selected--;
            } break;

            case CCTRL('g'):
            case 27: {
                dont_echo = true;
                running   = false;
            } break;

            // DEL (what most terminals actually send)
            // ^H
            case KEY_BACKSPACE:
            case 127:
            case '\b': {
                changed = true;
                forward_to_readline(127);
            } break;

            default: {
                changed = true;
                forward_to_readline(ch);
            } break;
        }
    }

    endwin();
    rl_callback_handler_remove();

    if (dont_echo) return current_display->sz > 0 ? 0 : 1;

    if (current_display->sz > 0) {
        HStrView sel = current_display->dt[selected].entry;
        printf("%.*s\n", (int)sel.sz, sel.dt);
        return 0;
    }
    if (rl_line_buffer && *rl_line_buffer)
        printf("%s\n", rl_line_buffer);
    return 1;
}
