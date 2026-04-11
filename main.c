#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1
#define NCURSES_WIDECHAR 1

#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <ncursesw/curses.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <wchar.h>
#include <wctype.h>
#include <pthread.h>
#include "helpa.h"
#include "config.h"

#define VERSION "0.1"
#define CCTRL(c) ((c) & 0x1E)

typedef struct { HStrView entry; i32 score; } Match;
typedef struct { Match *dt; size_t cp; size_t sz; } Entry;

typedef struct { char *entry; int len; } PreviewJob;

static Entry  entry         = {0};
static size_t selected      = 0;
static size_t scroll_offset = 0;

static bool rl_input_avail_flag = false;
static u8   rl_input_char       = 0;
static bool running             = true;
static bool dont_echo           = false;

static size_t mx = 80, my = 24;

static WINDOW *list_win    = NULL;
static WINDOW *preview_win = NULL;
static char   *preview_cmd = NULL;
static float   list_ratio  = 0.4f;

static pthread_t       preview_thread;
static pthread_mutex_t preview_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tty_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  preview_cond  = PTHREAD_COND_INITIALIZER;
static PreviewJob      preview_pending = {0};
static bool            preview_quit    = false;

static void run_preview(const char *entry_str, int entry_len);

static void print_help() {
    printf(
    "tmenu <flag> [stdin]\n"
    "  FLAG:\n"
    "    -bg [-1..7]      # color from the terminal\n"
    "    -fg [-1..7]      # color from the terminal\n"
    "    -prompt [str]    # prompt string\n"
    "    -char [chr]      # split character\n"
    "    -preview [cmd]   # preview command\n"
    "                     #   {}  = selected entry\n"
    "                     #   {W} = preview pane pixel width\n"
    "                     #   {H} = preview pane pixel height\n"
    "    -ratio [0.1-0.9] # list pane width fraction (default 0.4)\n"
    "    -help\n"
    "    -version\n"
    );
    exit(0);
}

static void print_version() {
    printf("tmenu version %s\n", VERSION);
    exit(0);
}

static int  rl_input_avail(void)    { return rl_input_avail_flag; }
static int  rl_getc_cb(FILE *dummy) { (void)dummy; rl_input_avail_flag = false; return rl_input_char; }
static void redisplay(void)         {}

static void forward_to_readline(int c) {
    rl_input_char       = (unsigned char)c;
    rl_input_avail_flag = true;
    rl_callback_read_char();
}

static void line_handler(char *line) {
    if (!line) dont_echo = true;
    running = false;
}

static size_t strnwidth(const char *s, size_t n, size_t offset) {
    mbstate_t st; wchar_t wc; size_t wc_len, width = 0;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < n; ) {
        wc_len = mbrtowc(&wc, s + i, n - i, &st);
        if (wc_len == 0) break;
        if (wc_len == (size_t)-1 || wc_len == (size_t)-2) {
            width++; i++; memset(&st, 0, sizeof st); continue;
        }
        if (wc == '\t') width = ((width + offset + 8) & ~7) - offset;
        else { int w = wcwidth(wc); width += iswcntrl(wc) ? 2 : (w > 0 ? (size_t)w : 0); }
        i += wc_len;
    }
    return width;
}

static int hstrview_score(HStrView *str, HStrView *cont) {
    if (cont->sz == 0) return 0;
    if (cont->sz > str->sz) return 9999;
    int best = 9999;
    for (size_t i = 0; i <= str->sz - cont->sz; i++) {
        bool match = true;
        for (size_t j = 0; j < cont->sz; j++)
            if (str->dt[i+j] != cont->dt[j]) { match = false; break; }
        if (match) {
            int score = (int)i + (int)(str->sz - cont->sz);
            if (score < best) best = score;
        }
    }
    return best;
}

static int match_cmp(const void *a, const void *b) {
    const Match *ma = (const Match*)a, *mb = (const Match*)b;
    if (ma->score != mb->score) return ma->score - mb->score;
    size_t min = ma->entry.sz < mb->entry.sz ? ma->entry.sz : mb->entry.sz;
    int cmp = memcmp(ma->entry.dt, mb->entry.dt, min);
    return cmp != 0 ? cmp : (int)(ma->entry.sz - mb->entry.sz);
}

static void reset_selected(Entry *cur) {
    if (cur->sz > 0 && cur->sz <= selected)      selected      = cur->sz - 1;
    if (cur->sz > 0 && cur->sz <= scroll_offset) scroll_offset = cur->sz - 1;
}

static void split_entry(HStr *str, char split) {
    u8 *before = str->dt;
    helpa_da_foreach(*str, c) {
        if (*c == split) {
            HStrView sv = { .dt = before, .sz = (i32)(c - before) };
            helpa_da_append(entry, ((Match){ .entry = sv, .score = 0 }));
            before = c + 1;
        }
    }
    if (before < str->dt + str->sz) {
        HStrView sv = { .dt = before, .sz = (i32)((str->dt + str->sz) - before) };
        if (sv.sz <= 0 || (sv.sz == 1 && isspace(*sv.dt))) return;
        helpa_da_append(entry, ((Match){ .entry = sv, .score = 0 }));
    }
}

static void create_windows(void) {
    if (!preview_cmd) return;

    int list_cols    = (int)(mx * list_ratio);
    int preview_cols = (int)mx - list_cols;
    int pane_rows    = (int)my - 1;

    if (list_cols < 4)    list_cols    = 4;
    if (preview_cols < 4) preview_cols = 4;
    if (pane_rows < 1)    pane_rows    = 1;

    if (list_win)    { delwin(list_win);    list_win    = NULL; }
    if (preview_win) { delwin(preview_win); preview_win = NULL; }

    list_win    = newwin(pane_rows, list_cols,    1, 0);
    preview_win = newwin(pane_rows, preview_cols, 1, list_cols);

    scrollok(list_win,    FALSE);
    scrollok(preview_win, TRUE);
}

static void resize_windows(void) {
    mx = (size_t)getmaxx(stdscr);
    my = (size_t)getmaxy(stdscr);
    create_windows();
}

static void get_cell_pixels(int *cell_w, int *cell_h) {
    struct winsize ws;
    *cell_w = 8;
    *cell_h = 16;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 0 && ws.ws_row > 0
        && ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
        *cell_w = ws.ws_xpixel / ws.ws_col;
        *cell_h = ws.ws_ypixel / ws.ws_row;
    }
}

static char *build_cmd(const char *entry_str, int entry_len,
                        const char *w_str, const char *h_str) {
    size_t buf_size = strlen(preview_cmd) + (size_t)entry_len * 4 + 64 + 6;
    char  *cmd      = malloc(buf_size);
    if (!cmd) return NULL;

    char *o = cmd;
    for (const char *p = preview_cmd; *p; ) {
        if (p[0] == '{' && p[1] == '}') {
            *o++ = '\'';
            for (int i = 0; i < entry_len; i++) {
                if (entry_str[i] == '\'') {
                    *o++ = '\''; *o++ = '\\'; *o++ = '\''; *o++ = '\'';
                } else {
                    *o++ = entry_str[i];
                }
            }
            *o++ = '\'';
            p += 2;
        } else if (p[0] == '{' && p[1] == 'W' && p[2] == '}') {
            o += sprintf(o, "%s", w_str); p += 3;
        } else if (p[0] == '{' && p[1] == 'H' && p[2] == '}') {
            o += sprintf(o, "%s", h_str); p += 3;
        } else {
            *o++ = *p++;
        }
    }
    memcpy(o, " 2>&1", 6);
    return cmd;
}

static void *preview_worker(void *arg) {
    (void)arg;
    while (true) {
        pthread_mutex_lock(&preview_mutex);
        while (!preview_pending.entry && !preview_quit)
            pthread_cond_wait(&preview_cond, &preview_mutex);

        if (preview_quit) {
            pthread_mutex_unlock(&preview_mutex);
            break;
        }

        PreviewJob job  = preview_pending;
        preview_pending = (PreviewJob){0};
        pthread_mutex_unlock(&preview_mutex);

        int py = preview_win ? getbegy(preview_win) : 1;
        int px = preview_win ? getbegx(preview_win) : (int)(mx * list_ratio);
        int pw = preview_win ? getmaxx(preview_win) : (int)mx - px;
        int ph = preview_win ? getmaxy(preview_win) : (int)my - 1;

        int cell_w, cell_h;
        get_cell_pixels(&cell_w, &cell_h);

        char w_str[16], h_str[16];
        snprintf(w_str, sizeof w_str, "%d", pw * cell_w);
        snprintf(h_str, sizeof h_str, "%d", ph * cell_h);

        char *cmd = build_cmd(job.entry, job.len, w_str, h_str);
        free(job.entry);
        if (!cmd) continue;

        FILE *fp = popen(cmd, "r");
        free(cmd);
        if (!fp) continue;

        char  *buf = NULL;
        size_t bsz = 0, blen = 0;
        char   tmp[4096];
        size_t nr;
        while ((nr = fread(tmp, 1, sizeof tmp, fp)) > 0) {
            if (blen + nr + 1 > bsz) {
                bsz = (blen + nr + 1) * 2;
                buf = realloc(buf, bsz);
            }
            memcpy(buf + blen, tmp, nr);
            blen += nr;
        }
        pclose(fp);

        if (!buf || blen == 0) { free(buf); continue; }
        buf[blen] = '\0';

        pthread_mutex_lock(&preview_mutex);
        bool stale = preview_pending.entry != NULL;
        pthread_mutex_unlock(&preview_mutex);

        if (stale) { free(buf); continue; }

        bool is_sixel = blen >= 2 && (unsigned char)buf[0] == 0x1B && buf[1] == 'P';

        pthread_mutex_lock(&tty_mutex);

        if (is_sixel) {
            FILE *tty = fopen("/dev/tty", "w");
            if (tty) {
                for (int r = 0; r < ph; r++)
                    fprintf(tty, "\033[%d;%dH\033[K", py + r + 1, px + 1);
                fprintf(tty, "\033[%d;%dH", py + 1, px + 1);
                fwrite(buf, 1, blen, tty);
                fprintf(tty, "\033[1;1H");
                fflush(tty);
                fclose(tty);
                if (list_win) { touchwin(list_win); wrefresh(list_win); }
                refresh();
            }
        } else {
            if (preview_win) {
                werase(preview_win);
                int   row  = 0;
                char *line = buf;
                char *end  = buf + blen;
                while (row < ph && line < end) {
                    char *nl = memchr(line, '\n', (size_t)(end - line));
                    int   ln = nl ? (int)(nl - line) : (int)(end - line);
                    if (ln > pw) ln = pw;
                    mvwaddnstr(preview_win, row, 0, line, ln);
                    row++;
                    line = nl ? nl + 1 : end;
                }
                wrefresh(preview_win);
            }
        }

        pthread_mutex_unlock(&tty_mutex);
        free(buf);
    }
    return NULL;
}

static void run_preview(const char *entry_str, int entry_len) {
    if (!preview_cmd) return;

    char *copy = malloc((size_t)entry_len + 1);
    if (!copy) return;
    memcpy(copy, entry_str, (size_t)entry_len);
    copy[entry_len] = '\0';

    pthread_mutex_lock(&preview_mutex);
    free(preview_pending.entry);
    preview_pending.entry = copy;
    preview_pending.len   = entry_len;
    pthread_cond_signal(&preview_cond);
    pthread_mutex_unlock(&preview_mutex);
}

int main(int argc, char **argv) {
    if (!setlocale(LC_ALL, "")) fprintf(stderr, "warning: failed to set locale\n");

    int   fg_color = -1;
    int   bg_color = SELECT_COLOR;
    char *prompt   = PROMPT;
    char  splitch  = '\n';

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-bg")      == 0 && i+1 < argc) { bg_color    = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-fg")      == 0 && i+1 < argc) { fg_color    = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-prompt")  == 0 && i+1 < argc) { prompt      = argv[++i]; }
        else if (strcmp(argv[i], "-char")    == 0 && i+1 < argc) { splitch     = *argv[++i]; }
        else if (strcmp(argv[i], "-preview") == 0 && i+1 < argc) { preview_cmd = argv[++i]; }
        else if (strcmp(argv[i], "-ratio")   == 0 && i+1 < argc) {
            list_ratio = (float)atof(argv[++i]);
            if (list_ratio < 0.1f) list_ratio = 0.1f;
            if (list_ratio > 0.9f) list_ratio = 0.9f;
        }
        else if (strcmp(argv[i], "-help")    == 0) print_help();
        else if (strcmp(argv[i], "-version") == 0) print_version();
        else fprintf(stderr, "warning: unknown flag '%s' ignored.\n", argv[i]);
    }

    HStr buffer = { .dt = malloc(1024), .cp = 1024, .sz = 0 };
    ssize_t n;
    do {
        n = read(0, buffer.dt + buffer.sz, buffer.cp - buffer.sz);
        if (n > 0) {
            buffer.sz += n;
            if (buffer.sz == buffer.cp) {
                buffer.cp *= 2;
                buffer.dt  = realloc(buffer.dt, buffer.cp);
            }
        }
    } while (n > 0);
    split_entry(&buffer, splitch);

    if (!isatty(0)) {
        FILE *tty_in  = fopen("/dev/tty", "r");
        FILE *tty_out = fopen("/dev/tty", "w");
        if (!tty_in || !tty_out) { fprintf(stderr, "warning: failed to open /dev/tty\n"); return 1; }
        set_term(newterm(NULL, tty_out, tty_in));
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

    mx = (size_t)getmaxx(stdscr);
    my = (size_t)getmaxy(stdscr);

    init_pair(1, fg_color, bg_color);
    create_windows();

    if (list_win)    keypad(list_win,    TRUE);
    if (preview_win) keypad(preview_win, TRUE);

    if (preview_cmd)
        pthread_create(&preview_thread, NULL, preview_worker, NULL);

    rl_catch_signals        = 0;
    rl_catch_sigwinch       = 0;
    rl_deprep_term_function = NULL;
    rl_prep_term_function   = NULL;
    rl_change_environment   = 0;
    rl_getc_function        = rl_getc_cb;
    rl_input_available_hook = rl_input_avail;
    rl_redisplay_function   = redisplay;
    rl_callback_handler_install(prompt, line_handler);

    Entry  search_results  = {0};
    Entry *current_display = &entry;
    bool   changed         = true;
    size_t last_selected   = (size_t)-1;

    while (running) {
        const char *line_buf     = rl_line_buffer ? rl_line_buffer : "";
        size_t      line_sz      = strlen(line_buf);
        size_t      prompt_width = strlen(prompt);

        size_t list_cols_sz = preview_cmd ? (size_t)(mx * list_ratio) : mx;
        size_t input_rows   = (prompt_width + strnwidth(line_buf, line_sz, prompt_width) + list_cols_sz - 1) / list_cols_sz;
        if (input_rows == 0) input_rows = 1;

        size_t max_visible = preview_cmd
            ? (my > 1 ? my - 1 : 1)
            : (my > input_rows ? my - input_rows : 0);

        if (changed) {
            if (line_sz > 0) {
                search_results.sz = 0;
                HStrView converted = { .dt = (u8*)line_buf, .sz = line_sz };
                helpa_da_foreach(entry, items) {
                    int score = hstrview_score(&items->entry, &converted);
                    if (score < 9999)
                        helpa_da_append(search_results, ((Match){ .entry = items->entry, .score = score }));
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

        WINDOW *lw        = list_win ? list_win : stdscr;
        size_t  lw_cols   = (size_t)getmaxx(lw);
        size_t  row_start = preview_cmd ? 0 : input_rows;

        werase(lw);

        size_t row = row_start, index = 0;
        helpa_da_foreach(*current_display, items) {
            if (index >= scroll_offset && index < scroll_offset + max_visible) {
                int len = (int)items->entry.sz;
                if ((size_t)len > lw_cols) len = (int)lw_cols;
                mvwaddnstr(lw, (int)row, 0, (const char *)items->entry.dt, len);
                if (index == selected)
                    mvwchgat(lw, (int)row, 0, -1, A_NORMAL, 1, NULL);
                row++;
            }
            index++;
        }

        mvaddstr(0, 0, prompt);
        addstr(line_buf);
        clrtoeol();

        size_t cursor_col = prompt_width + strnwidth(line_buf, (size_t)rl_point, prompt_width);
        if (cursor_col < mx) move(0, (int)cursor_col);

        pthread_mutex_lock(&tty_mutex);
        wrefresh(lw);
        refresh();
        pthread_mutex_unlock(&tty_mutex);

        if (preview_cmd && current_display->sz > 0 && selected != last_selected) {
            HStrView sel = current_display->dt[selected].entry;
            run_preview((const char *)sel.dt, (int)sel.sz);
            last_selected = selected;
        } else if (preview_cmd && current_display->sz == 0 && last_selected != (size_t)-1) {
            if (preview_win) {
                pthread_mutex_lock(&tty_mutex);
                werase(preview_win);
                wrefresh(preview_win);
                pthread_mutex_unlock(&tty_mutex);
            }
            last_selected = (size_t)-1;
        }

        wint_t wch = 0;
        int    ret = get_wch(&wch);
        if (!running) break;

        if (ret == ERR) {
        } else if (ret == KEY_CODE_YES) {
            switch ((int)wch) {
                case KEY_RESIZE:    resize_windows(); wclear(stdscr); break;
                case KEY_BACKSPACE: changed = true; forward_to_readline(127); break;
            }
        } else {
            switch ((int)wch) {
                case CCTRL('n'):
                    selected = (current_display->sz > 0 && selected < current_display->sz - 1)
                               ? selected + 1 : 0;
                    wclear(lw);
                    break;
                case CCTRL('p'):
                    selected = selected > 0
                               ? selected - 1
                               : (current_display->sz > 0 ? current_display->sz - 1 : 0);
                    wclear(lw);
                    break;
                case 27:
                    dont_echo = true; running = false; break;
                case 127: case '\b':
                    changed = true; forward_to_readline(127); break;
                default:
                    changed = true;
                    if ((wchar_t)wch < 0x80) {
                        forward_to_readline((unsigned char)wch);
                    } else {
                        char buf[MB_LEN_MAX + 1]; mbstate_t st; memset(&st, 0, sizeof st);
                        size_t nb = wcrtomb(buf, (wchar_t)wch, &st);
                        if (nb != (size_t)-1)
                            for (size_t i = 0; i < nb; i++)
                                forward_to_readline((unsigned char)buf[i]);
                    }
                    break;
            }
        }
    }

    if (list_win)    { delwin(list_win);    list_win    = NULL; }
    if (preview_win) { delwin(preview_win); preview_win = NULL; }

    if (preview_cmd) {
        pthread_mutex_lock(&preview_mutex);
        free(preview_pending.entry);
        preview_pending.entry = NULL;
        preview_quit = true;
        pthread_cond_signal(&preview_cond);
        pthread_mutex_unlock(&preview_mutex);
        pthread_join(preview_thread, NULL);
    }

    endwin();
    rl_callback_handler_remove();

    if (dont_echo) return current_display->sz > 0 ? 0 : 1;

    if (current_display->sz > 0) {
        HStrView sel = current_display->dt[selected].entry;
        printf("%.*s\n", (int)sel.sz, sel.dt);
        return 0;
    }
    if (rl_line_buffer && *rl_line_buffer) printf("%s\n", rl_line_buffer);
    return 1;
}
