CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -Iadapters -O2

SRC_CORE = src/ecat_foe_core.c
SRC_SSC  = adapters/ecat_foe_ssc.c
SRC_SOES = adapters/ecat_foe_soes.c

SRCS = $(SRC_CORE) $(SRC_SSC) $(SRC_SOES)
TEST_SRC = tests/test_ecat_foe.c

TEST_BIN = tests/test_runner

.PHONY: all clean test

all: test

$(TEST_BIN): $(SRCS) $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_BIN)
	@./$(TEST_BIN)

clean:
	rm -f $(TEST_BIN) *.efw tests/*.efw
