CC     = gcc
CFLAGS = -Wall -Wextra -O2

.PHONY: all test clean

all: shell

shell: shell.c parser.c mpc.c parser.h mpc.h
	$(CC) $(CFLAGS) shell.c parser.c mpc.c -o shell

test: shell
	./run_examples.sh

clean:
	rm -f shell
