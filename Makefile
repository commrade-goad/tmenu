debug: main.c
	cc -o tmenu main.c -Wall -Wextra -lncursesw -ltinfow -lreadline -ggdb

release: main.c
	cc -o tmenu main.c -Wall -Wextra -lncursesw -ltinfow -lreadline -march=native -flto -O2

runner: runner.c
	cc -o tmenu_runner runner.c -Wall -Wextra -O2 -flto -march=native
