CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -O2
PYTHON ?= python3

SRC = src/ecat_foe_efw.c
TEST_SRC = tests/test_efw.c
TEST_BIN = tests/test_runner

.PHONY: all clean test

all: test

$(TEST_BIN): $(SRC) $(TEST_SRC) include/ecat_foe_efw.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(TEST_SRC)

test: $(TEST_BIN)
	@./$(TEST_BIN)
	@$(PYTHON) tests/test_efw_pack.py

clean:
	rm -f $(TEST_BIN) *.efw tests/*.efw
