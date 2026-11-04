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

SOES_DIR ?= /tmp/soes_src/soes
PDF_SOURCES_DIR ?= $(HOME)/ecat-sources
PDF_FILE ?= $(PDF_SOURCES_DIR)/00001909A.pdf

.PHONY: all clean test test-asan vectors check-quotes check-pdf-quotes gen-pdf-table

all: test

vectors:
	@$(PYTHON) tests/generate_vectors.py

$(TEST_EFW_BIN): $(SRC_EFW) $(TEST_EFW_SRC) include/ecat_foe_efw.h
	$(CC) $(CFLAGS) -o $@ $(SRC_EFW) $(TEST_EFW_SRC)

$(TEST_CORE_BIN): $(SRC_CORE) $(TEST_CORE_SRC) include/ecat_foe_core.h include/ecat_foe_target.h include/ecat_foe_soes.h tests/mock_driver.h
	$(CC) $(CFLAGS) -o $@ $(SRC_CORE) $(TEST_CORE_SRC)

check-quotes:
	@echo ">>> 执行 SOES 源码引文三方核验 (来源目录: $(SOES_DIR)) <<<"
	@if [ ! -d "$(SOES_DIR)" ]; then \
		echo "[FATAL] SOES 源码目录不存在: $(SOES_DIR)"; \
		echo "可通过传递参数指定: make check-quotes SOES_DIR=/path/to/soes"; \
		exit 1; \
	fi
	@$(PYTHON) tools/verify_soes_quotes.py --soes-dir $(SOES_DIR)

check-pdf-quotes:
	@echo ">>> 执行厂商 PDF 引文与跨页章节归属严格核验 <<<"
	@if [ ! -f "$(PDF_FILE)" ]; then \
		echo "[FATAL] 厂商 PDF 不存在: $(PDF_FILE)"; \
		exit 1; \
	fi
	@$(PYTHON) tools/gen_pdf_cite_table.py --check
	@$(PYTHON) tools/verify_pdf_quotes.py --pdf-path $(PDF_FILE)

gen-pdf-table:
	@$(PYTHON) tools/gen_pdf_cite_table.py --update

test: vectors $(TEST_EFW_BIN) $(TEST_CORE_BIN)
	@which pdftotext >/dev/null 2>&1 || (echo "错误: pdftotext 缺失，请安装 poppler-utils (macOS: brew install poppler)" && exit 1)
	@./$(TEST_EFW_BIN)
	@$(PYTHON) tests/test_efw_pack.py
	@./$(TEST_CORE_BIN)
	@$(PYTHON) tests/test_verify_quotes.py
	@$(PYTHON) tests/test_verify_pdf_quotes.py

# ASan 内存与未定义行为核验目标
# 历史说明: 早期测试夹具 mock_driver_t 内部包含 1MB 模拟内存缓冲区，在测试用例作为局部变量定义时，
# Clang -O2 将数十个测试用例内联至 main()，叠加 ASan 栈红色区导致主线程栈帧超过 macOS 默认 8MB 限制引发 stack-overflow (退出码 134)。
# 本库核心代码本身为零动态分配且各函数栈帧极小。测试套件已将 1MB 模拟硬件缓冲区置入静态存储区 (BSS)，不再依赖 -fno-inline。
test-asan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -g"

clean:
	rm -rf $(TEST_EFW_BIN) $(TEST_CORE_BIN) tests/test_runner* tests/*.dSYM *.dSYM *.efw tests/*.efw
