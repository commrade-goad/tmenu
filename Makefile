debug: main.c
	cc -o tmenu main.c -Wall -Wextra -lncursesw -ltinfow -lreadline -ggdb

release: main.c
	cc -o tmenu main.c -Wall -Wextra -lncursesw -ltinfow -lreadline -march=native -flto -O2
