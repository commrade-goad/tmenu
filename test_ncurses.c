#include <ncursesw/curses.h>
#include <stdio.h>

int main() {
    initscr();
    cbreak();
    noecho();

    int cols = COLS;
    int lines = LINES;

    int mx = cols / 2;
    WINDOW *win = newwin(lines, mx, 0, 0);
    keypad(win, TRUE);

    waddstr(win, "Hello left pane");
    wrefresh(win);

    // Draw to right pane using ANSI
    printf("\033[%d;%dHHello right pane!", 5, mx + 2);
    fflush(stdout);

    wgetch(win);

    endwin();
    return 0;
}
