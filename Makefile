CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -Itests/stubs -O2
PYTHON ?= python3

# 阶段 A: efw 容器解析测试
SRC_EFW = src/ecat_foe_efw.c
TEST_EFW_SRC = tests/test_efw.c
TEST_EFW_BIN = tests/test_runner_efw

# 阶段 B: 核心升级状态机、目标驱动与 SOES 适配测试
SRC_CORE = src/ecat_foe_efw.c src/ecat_foe_core.c src/adapters/ecat_foe_soes.c
TEST_CORE_SRC = tests/mock_driver.c tests/test_core.c
TEST_CORE_BIN = tests/test_runner_core

.PHONY: all clean test test-asan vectors

all: test

vectors:
	@$(PYTHON) tests/generate_vectors.py

$(TEST_EFW_BIN): $(SRC_EFW) $(TEST_EFW_SRC) include/ecat_foe_efw.h
	$(CC) $(CFLAGS) -o $@ $(SRC_EFW) $(TEST_EFW_SRC)

$(TEST_CORE_BIN): $(SRC_CORE) $(TEST_CORE_SRC) include/ecat_foe_core.h include/ecat_foe_target.h include/ecat_foe_soes.h tests/mock_driver.h
	$(CC) $(CFLAGS) -o $@ $(SRC_CORE) $(TEST_CORE_SRC)

test: vectors $(TEST_EFW_BIN) $(TEST_CORE_BIN)
	@./$(TEST_EFW_BIN)
	@$(PYTHON) tests/test_efw_pack.py
	@./$(TEST_CORE_BIN)

test-asan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -g"

clean:
	rm -rf $(TEST_EFW_BIN) $(TEST_CORE_BIN) tests/test_runner* tests/*.dSYM *.dSYM *.efw tests/*.efw
