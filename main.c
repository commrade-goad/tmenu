#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1
#define NCURSES_WIDECHAR 1

#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>
// #include <curses.h>
#include <ncursesw/curses.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <wchar.h>
#include <wctype.h>
#include "helpa.h"
#include "config.h"

#define VERSION "0.1"
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

static void print_help() {
    printf(
    "tmenu <flag> [stdin]\n"
    "  NOTE: right now it only support input from stdin.\n"
    "  FLAG:\n"
    "    -bg [-1..7]   # color from the terminal\n"
    "    -fg [-1..7]   # color from the terminal\n"
    "    -prompt [str] # a string for the prompt\n"
    "    -char [chr]   # a char that will be used for splitting\n"
    "    -preview [cmd]       # a command to run for the preview\n"
    "    -preview-ratio [r]   # preview ratio (e.g., 3:4), default 3:4\n"
    "    -help         # print help\n"
    "    -version      # print version\n"
    );
    exit(0);
}
static void print_version() {
    printf("tmenu version %s\n", VERSION);
    exit(0);
}

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
    for (size_t i = 0; i < n; ) {
        wc_len = mbrtowc(&wc, s + i, n - i, &st);
        if (wc_len == 0) break;
        if (wc_len == (size_t)-1 || wc_len == (size_t)-2) {
            width++;
            i++;
            memset(&st, 0, sizeof st);
            continue;
        }
        if (wc == '\t')
        width = ((width + offset + 8) & ~7) - offset;
        else {
            int w = wcwidth(wc);
            width += (iswcntrl(wc)) ? 2 : (w > 0 ? (size_t)w : 0);
        }
        i += wc_len;
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

static HStr execute_preview(const char *cmd, HStrView selected_item) {
    HStr out = {0};
    if (!cmd) return out;

    // Replace "{}" with the selected_item
    HStr final_cmd = {0};
    const char *placeholder = strstr(cmd, "{}");
    if (placeholder) {
        size_t prefix_len = placeholder - cmd;
        HStrView prefix = { .dt = (const u8*)cmd, .sz = prefix_len };
        hstr_append_view(&final_cmd, prefix);

        // Escape the selected item for shell just to be safe, or just append it
        // To be safe with shell, wrap in single quotes and replace ' with '\''
        hstr_push(&final_cmd, '\'');
        for (size_t i = 0; i < selected_item.sz; i++) {
            if (selected_item.dt[i] == '\'') {
                hstr_append_cstr(&final_cmd, "'\\''");
            } else {
                hstr_push(&final_cmd, selected_item.dt[i]);
            }
        }
        hstr_push(&final_cmd, '\'');

        HStrView suffix = { .dt = (const u8*)(placeholder + 2), .sz = strlen(placeholder + 2) };
        hstr_append_view(&final_cmd, suffix);
    } else {
        hstr_append_cstr(&final_cmd, cmd);
    }

    // Ensure stderr is discarded to prevent mixing output
    hstr_append_cstr(&final_cmd, " 2>/dev/null");

    FILE *f = popen((const char *)final_cmd.dt, "r");
    if (f) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            HStrView v = { .dt = (const u8*)buf, .sz = n };
            hstr_append_view(&out, v);
        }
        pclose(f);
    }

    hstr_free(&final_cmd);
    return out;
}

int main(int argc, char **argv) {
    if (!setlocale(LC_ALL, "")) fprintf(stderr, "warning: failed to set locale\n");

    int fg_color = FG_SELECT_COLOR;
    int bg_color = SELECT_COLOR;
    char *prompt = PROMPT;
    char splitch = '\n';

    char *preview_cmd = NULL;
    int ratio_menu = 3;
    int ratio_preview = 4;

    if (argc > 1) {
        for(int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-bg") == 0 && i + 1 < argc) {
                bg_color = atoi(argv[++i]);
                if (*argv[i] != '0' && bg_color == 0) {
                    fprintf(stderr, "warning: invalid bg color `%s` using the default one.\n", argv[i]);
                    bg_color = SELECT_COLOR;
                }
                continue;
            } else
            if (strcmp(argv[i], "-fg") == 0 && i + 1 < argc) {
                fg_color = atoi(argv[++i]);
                if (*argv[i] != '0' && fg_color == 0) {
                    fprintf(stderr, "warning: invalid fg color `%s` using the default one.\n", argv[i]);
                    fg_color = -1;
                }
                continue;
            } else
            if (strcmp(argv[i], "-prompt") == 0 && i + 1 < argc) {
                prompt = argv[++i];
                continue;
            } else
            if (strcmp(argv[i], "-char") == 0 && i + 1 < argc) {
                splitch = *argv[++i];
                continue;
            } else
            if (strcmp(argv[i], "-preview") == 0 && i + 1 < argc) {
                preview_cmd = argv[++i];
                continue;
            } else
            if (strcmp(argv[i], "-preview-ratio") == 0 && i + 1 < argc) {
                char *ratio_str = argv[++i];
                if (sscanf(ratio_str, "%d:%d", &ratio_menu, &ratio_preview) != 2 || ratio_menu <= 0 || ratio_preview < 0 || (ratio_menu + ratio_preview) <= 0) {
                    fprintf(stderr, "warning: invalid preview ratio format, using 3:4.\n");
                    ratio_menu = 3;
                    ratio_preview = 4;
                }
                continue;
            } else
            if (strcmp(argv[i], "-help") == 0) print_help();
            else if (strcmp(argv[i], "-version") == 0) print_version();
            else {
                fprintf(stderr, "warning: invalid or unfinished flag '%s' ignored.\n", argv[i]);
            }
        }
    }

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

    split_entry(&buffer, splitch);

    // Init ncurses
    SCREEN *scr = NULL;
    if (!isatty(0)) {
        FILE *tty_in  = fopen("/dev/tty", "r");
        FILE *tty_out = fopen("/dev/tty", "w");
        if (!tty_in || !tty_out) {
            fprintf(stderr, "warning: failed to open /dev/tty\n");
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

    size_t preview_w = 0;
    size_t menu_w = mx;

    if (preview_cmd != NULL) {
        int total_ratio = ratio_menu + ratio_preview;
        menu_w = (mx * ratio_menu) / total_ratio;
        if (menu_w == 0) menu_w = 1;
        preview_w = mx - menu_w;
    }

    init_pair(1, fg_color, bg_color);

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

    rl_callback_handler_install(prompt, line_handler);

    Entry search_results   = {0};
    Entry *current_display = &entry;
    bool changed = true;

    while (running) {
        const char *line_buf = rl_line_buffer ? rl_line_buffer : "";
        size_t      line_sz  = strlen(line_buf);

        size_t prompt_width = strlen(prompt);
        size_t input_len    = prompt_width + strnwidth(line_buf, line_sz, prompt_width);
        size_t input_rows   = (input_len + menu_w - 1) / menu_w;
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
            wclear(stdscr);
            changed = false;
        }

        reset_selected(current_display);

        if (selected < scroll_offset)
            scroll_offset = selected;
        else if (selected >= scroll_offset + max_visible && max_visible > 0)
            scroll_offset = selected - max_visible + 1;

        // wclear was here

        size_t row = input_rows, index = 0;
        helpa_da_foreach(*current_display, items) {
            if (index >= scroll_offset && index < scroll_offset + max_visible) {
                int len = (int)items->entry.sz;
                if ((size_t)len > menu_w) len = (int)menu_w;
                mvaddnstr((int)row, 0, (const char *)items->entry.dt, len);
                if (index == selected)
                    mvchgat((int)row, 0, (int)menu_w, A_NORMAL, 1, NULL);
                row++;
            }
            index++;
        }

        mvaddstr(0, 0, prompt);
        addstr(line_buf);

        // Clear to end of line, but only up to menu_w
        int cur_x = getcurx(stdscr);
        for (size_t i = cur_x; i < menu_w; i++) {
            addch(' ');
        }

        size_t cursor_col = prompt_width +
            strnwidth(line_buf, (size_t)rl_point, prompt_width);
        if (cursor_col < menu_w)
            move(0, (int)cursor_col);

        refresh();

        // Draw the preview window
        if (preview_cmd != NULL && preview_w > 0) {
            // Clear the preview area using ANSI escape codes
            for (size_t y = 0; y < my; y++) {
                printf("\033[%zu;%zuH\033[K", y + 1, menu_w + 1);
            }

            if (current_display->sz > 0) {
                HStrView sel = current_display->dt[selected].entry;
                HStr preview_out = execute_preview(preview_cmd, sel);

                if (preview_out.sz > 0) {
                    size_t p_y = 1;
                    size_t p_x = menu_w + 1;
                    printf("\033[%zu;%zuH", p_y, p_x);

                    size_t cur_x_offset = 0;
                    int escape_state = 0; // 0 = normal, 1 = saw ESC, 2 = CSI ([), 3 = DCS (P) or OSC (]), 4 = string processing

                    for (size_t i = 0; i < preview_out.sz; i++) {
                        u8 c = preview_out.dt[i];

                        if (escape_state == 0 && c == '\033') {
                            escape_state = 1;
                        }

                        if (escape_state > 0) {
                            putchar(c);
                            if (escape_state == 1) {
                                if (c == '[') escape_state = 2; // CSI
                                else if (c == 'P' || c == ']' || c == '_' || c == '^') escape_state = 4; // DCS, OSC, APC, PM
                                else if (c != '\033') escape_state = 0; // Other short escapes like ESC M
                            } else if (escape_state == 2) {
                                if (isalpha(c)) escape_state = 0; // End of CSI
                            } else if (escape_state == 4) {
                                // Looking for String Terminator (ESC \) or BEL (\x07)
                                if (c == '\x07') escape_state = 0;
                                else if (c == '\\' && i > 0 && preview_out.dt[i-1] == '\033') escape_state = 0;
                            }
                        } else {
                            if (c == '\n') {
                                p_y++;
                                if (p_y > my) break;
                                printf("\033[%zu;%zuH", p_y, p_x);
                                cur_x_offset = 0;
                            } else if (c == '\r') {
                                printf("\033[%zu;%zuH", p_y, p_x);
                                cur_x_offset = 0;
                            } else {
                                if (cur_x_offset < preview_w) {
                                    putchar(c);
                                    cur_x_offset++;
                                }
                            }
                        }
                    }
                }
                hstr_free(&preview_out);
            }

            // Move cursor back to input line
            if (cursor_col < menu_w)
                printf("\033[1;%zuH", cursor_col + 1);
            else
                printf("\033[1;%zuH", menu_w + 1);

            fflush(stdout);
        }

        wint_t wch = 0;
        int ret = get_wch(&wch);
        if (!running) break;

        if (ret == ERR) {
            /* timeout — nothing to do */
        } else if (ret == KEY_CODE_YES) {
            /* special/function key */
            switch ((int)wch) {
                case KEY_RESIZE: {
                    mx = getmaxx(stdscr);
                    my = getmaxy(stdscr);
                    if (preview_cmd != NULL) {
                        int total_ratio = ratio_menu + ratio_preview;
                        menu_w = (mx * ratio_menu) / total_ratio;
                        if (menu_w == 0) menu_w = 1;
                        preview_w = mx - menu_w;
                    } else {
                        menu_w = mx;
                        preview_w = 0;
                    }
                } break;
                case KEY_BACKSPACE: {
                    changed = true;
                    forward_to_readline(127);
                } break;
                default: break;
            }
        } else {
            /* ret == OK: real character (may be wide / emoji) */
            switch ((int)wch) {
                case CCTRL('n'): {
                    if (current_display->sz > 0 && selected < current_display->sz - 1) {
                        selected++;
                    } else selected = 0;
                    wclear(stdscr);
                } break;

                case CCTRL('p'): {
                    if (selected > 0) {
                        selected--;
                    } else selected = current_display->sz -1;
                    wclear(stdscr);
                } break;

                case 27: {
                    dont_echo = true;
                    running   = false;
                } break;

                case 127:
                case '\b': {
                    changed = true;
                    forward_to_readline(127);
                } break;

                default: {
                    changed = true;
                    if ((wchar_t)wch < 0x80) {
                        /* plain ASCII — feed directly */
                        forward_to_readline((unsigned char)wch);
                    } else {
                        /* multi-byte: encode to UTF-8 and feed byte-by-byte */
                        char buf[MB_LEN_MAX + 1];
                        mbstate_t st;
                        memset(&st, 0, sizeof st);
                        size_t n = wcrtomb(buf, (wchar_t)wch, &st);
                        if (n != (size_t)-1)
                            for (size_t i = 0; i < n; i++)
                                forward_to_readline((unsigned char)buf[i]);
                    }
                } break;
            }
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
