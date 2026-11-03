CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -O2
PYTHON ?= python3

SRC_EFW = src/ecat_foe_efw.c
TEST_EFW_SRC = tests/test_efw.c
TEST_EFW_BIN = tests/test_runner_efw

.PHONY: all clean test test-asan vectors

all: test

vectors:
	@$(PYTHON) tests/generate_vectors.py

$(TEST_EFW_BIN): $(SRC_EFW) $(TEST_EFW_SRC) include/ecat_foe_efw.h
	$(CC) $(CFLAGS) -o $@ $(SRC_EFW) $(TEST_EFW_SRC)

test: vectors $(TEST_EFW_BIN)
	@./$(TEST_EFW_BIN)
	@$(PYTHON) tests/test_efw_pack.py

test-asan:
	$(MAKE) clean
	$(MAKE) test CFLAGS="$(CFLAGS) -fsanitize=address,undefined -g"

clean:
	rm -f $(TEST_EFW_BIN) tests/test_runner *.efw tests/*.efw
