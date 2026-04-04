all: main.c
	cc -o main main.c -Wall -Wextra -ggdb -lncurses -ltinfo
