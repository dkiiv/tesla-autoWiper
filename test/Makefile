# Host-side unit tests for the pure-C protocol/policy code.
# Needs only a C compiler:   make -C test          (or ./idf.sh test)
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
SRC      = test_main.c ../main/protocol.c ../main/wiper_policy.c

.PHONY: test clean
test: test_runner
	./test_runner

test_runner: $(SRC) ../main/protocol.h ../main/wiper_policy.h
	$(CC) $(CFLAGS) -I../main -o $@ $(SRC)

clean:
	rm -f test_runner
