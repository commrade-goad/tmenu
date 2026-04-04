all: main.c
	cc -o tmenu main.c -Wall -Wextra -lncurses -ltinfo -lreadline -march=native -flto -O2
