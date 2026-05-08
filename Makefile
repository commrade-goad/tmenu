CFLAGS = -Wall -Wextra
LIBFLAGS = -lncursesw -ltinfo -lreadline

release: main.c
	cc -o tmenu main.c $(CFLAGS) -march=native -flto -O2 $(LIBFLAGS)

debug: main.c
	cc -o tmenu main.c $(CFLAGS) -ggdb $(LIBFLAGS)

runner: runner.c
	cc -o tmenu_runner runner.c $(CFLAGS) -march=native -flto -O2

.PHONY: release debug runner
