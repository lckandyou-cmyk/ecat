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

# 阶段 C: 真实 SOES 源码宿主独立 Harness 测试 (按需检测外部 SOES 源码)
TEST_SOES_HARNESS_SRC = tests/test_soes_real_harness.c
TEST_SOES_HARNESS_BIN = tests/test_runner_soes_real
SOES_CFLAGS = $(filter-out -Itests/stubs,$(CFLAGS)) -Wno-zero-length-array -Wno-c11-extensions -Wno-strict-prototypes -Itests/soes_bsp -I$(SOES_DIR)

# 阶段 D: 宿主机参考示例与驱动模板源码
EXAMPLES_HOST_DEMO_SRC = examples/host_demo/main.c src/ecat_foe_efw.c src/ecat_foe_core.c
EXAMPLES_DRIVER_TEMPLATE_SRC = examples/driver_template/ecat_foe_driver_template.c

SOES_DIR ?= /tmp/soes_src/soes
PDF_SOURCES_DIR ?= $(HOME)/ecat-sources
PDF_FILE ?= $(PDF_SOURCES_DIR)/00001909A.pdf

.PHONY: all clean test test-asan test-soes test-examples test-sentinels vectors check-quotes check-pdf-quotes gen-pdf-table

all: test

vectors:
	@$(PYTHON) tests/generate_vectors.py

$(TEST_EFW_BIN): $(SRC_EFW) $(TEST_EFW_SRC) include/ecat_foe_efw.h
	$(CC) $(CFLAGS) -o $@ $(SRC_EFW) $(TEST_EFW_SRC)

$(TEST_CORE_BIN): $(SRC_CORE) $(TEST_CORE_SRC) include/ecat_foe_core.h include/ecat_foe_target.h include/ecat_foe_soes.h tests/mock_driver.h
	$(CC) $(CFLAGS) -o $@ $(SRC_CORE) $(TEST_CORE_SRC)

$(TEST_SOES_HARNESS_BIN): $(SRC_CORE) $(TEST_SOES_HARNESS_SRC) tests/mock_driver.c tests/soes_bsp/cc.h tests/soes_bsp/options.h tests/soes_bsp/esc_coe.h $(SOES_DIR)/esc_foe.c
	$(CC) $(SOES_CFLAGS) -o $@ $(SRC_CORE) $(TEST_SOES_HARNESS_SRC) tests/mock_driver.c $(SOES_DIR)/esc_foe.c

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

test: vectors $(TEST_EFW_BIN) $(TEST_CORE_BIN) test-examples
	@which pdftotext >/dev/null 2>&1 || (echo "错误: pdftotext 缺失，请安装 poppler-utils (macOS: brew install poppler)" && exit 1)
	@./$(TEST_EFW_BIN)
	@$(PYTHON) tests/test_efw_pack.py
	@./$(TEST_CORE_BIN)
	@echo "--- 运行普通测试二进制受限栈回归核验 (8MB 与 1MB 栈) ---"
	@(ulimit -s 8192 && ./$(TEST_CORE_BIN) >/dev/null)
	@(ulimit -s 1024 && ./$(TEST_CORE_BIN) >/dev/null)
	@echo "  [PASS] 普通 $$(basename $(TEST_CORE_BIN)) 在 8MB 与 1MB 栈下运行正常"
	@(ulimit -s 8192 && ./$(TEST_EFW_BIN) >/dev/null)
	@(ulimit -s 1024 && ./$(TEST_EFW_BIN) >/dev/null)
	@echo "  [PASS] 普通 $$(basename $(TEST_EFW_BIN)) 在 8MB 与 1MB 栈下运行正常"
	@$(PYTHON) tests/test_verify_quotes.py
	@$(PYTHON) tests/test_verify_pdf_quotes.py

test-sentinels:
	@$(PYTHON) tests/test_repo_sentinels.py

test-examples:
	@TMP_BUILD_DIR=$$(mktemp -d /tmp/foe_build_examples_XXXXXX) && \
	trap 'rm -rf "$$TMP_BUILD_DIR"' EXIT INT TERM && \
	echo "--- 编译并运行宿主参考示例 (构建目录: $$TMP_BUILD_DIR) ---" && \
	$(CC) $(CFLAGS) -o "$$TMP_BUILD_DIR/host_demo" $(EXAMPLES_HOST_DEMO_SRC) && \
	$(PYTHON) -c 'open("'$$TMP_BUILD_DIR'/raw_payload.bin", "wb").write(bytes([0x55] * 64))' && \
	$(PYTHON) tools/efw_pack.py pack \
		--bin "$$TMP_BUILD_DIR/raw_payload.bin" \
		--project-id 0x10010001 \
		--project-name SERVO_DRV \
		--hardware-id 0x00000001 \
		--hw-ver-min 1 \
		--hw-ver-max 1 \
		--target MCU \
		--fw-version 1.0.0.0 \
		--foe-packet-size 64 \
		--out "$$TMP_BUILD_DIR/packed_padded.efw" >/dev/null && \
	"$$TMP_BUILD_DIR/host_demo" "$$TMP_BUILD_DIR/packed_padded.efw" > "$$TMP_BUILD_DIR/run_output.tmp" && \
	diff -u examples/host_demo/expected_output.txt "$$TMP_BUILD_DIR/run_output.tmp" && \
	echo "  [PASS] host_demo 输出与 expected_output.txt 逐行比对完全匹配" && \
	(ulimit -s 8192 && "$$TMP_BUILD_DIR/host_demo" "$$TMP_BUILD_DIR/packed_padded.efw" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_BUILD_DIR/host_demo" "$$TMP_BUILD_DIR/packed_padded.efw" >/dev/null) && \
	echo "  [PASS] host_demo 在 8MB 与 1MB 栈下运行正常" && \
	echo "--- 运行 host_demo 外部隔离、同名哨兵保护与多实例并发验证 ---" && \
	$(PYTHON) tests/test_host_demo_isolation.py "$$TMP_BUILD_DIR/host_demo" "$$TMP_BUILD_DIR/packed_padded.efw" && \
	echo "--- 静态编译检查驱动模板 (在独立临时构建目录) ---" && \
	$(CC) $(CFLAGS) -c -o "$$TMP_BUILD_DIR/driver_template.o" $(EXAMPLES_DRIVER_TEMPLATE_SRC) && \
	echo "  [PASS] driver_template 编译检查通过 (-Wall -Wextra -Werror -pedantic)"

test-soes:
	@if [ -z "$(SOES_DIR)" ] || [ ! -f "$(SOES_DIR)/esc_foe.c" ]; then \
		echo "错误: SOES_DIR 缺失或未包含 esc_foe.c: '$(SOES_DIR)'"; \
		echo "使用方法: make test-soes SOES_DIR=/path/to/soes"; \
		exit 1; \
	fi
	@$(PYTHON) tools/check_soes_hash.py --soes-dir "$(SOES_DIR)"
	@$(MAKE) $(TEST_SOES_HARNESS_BIN) CC="$(CC)" CFLAGS="$(CFLAGS)" SOES_DIR="$(SOES_DIR)"
	@./$(TEST_SOES_HARNESS_BIN)
	@(ulimit -s 8192 && ./$(TEST_SOES_HARNESS_BIN) >/dev/null)
	@(ulimit -s 1024 && ./$(TEST_SOES_HARNESS_BIN) >/dev/null)
	@echo "  [PASS] 真实 SOES Harness 在 8MB 与 1MB 栈下运行正常"

# ASan 内存与未定义行为核验目标
# 历史构建与环境条件复核说明:
# 1. 历史复核事实 (commit 9c78bc2 与 c1792c4):
#    - 在 macOS 下通过 GNU Make 执行 `make test-asan` 时，Make 为子进程赋予了较大栈上限 (实测为 65520KB)，测试退出码为 0；
#    - 但生成的同一 ASan 二进制脱离 make 时，在 commit c1792c4 实测：以 ulimit -s 8192 (8MB) 或 1024 (1MB) 独立执行，
#      均因多个测试用例将 1MB 模拟缓冲区分配于栈上且被 Clang -O2 内联至 main()，导致 stack-overflow 退出码 134，堆栈指向 main 入口。
# 2. 夹具修复与回归验证 (commit e31190d 起):
#    已将 tests/test_core.c 内测试夹具统一调整为静态存储 (static/BSS)。
#    回归核验：同一 ASan 二进制在 ulimit -s 8192 与 1024 下独立执行均顺利通过 (退出码 0)。
# 3. 栈帧尺寸限定：核心库代码 (src/ecat_foe_core.c) 为零动态分配，内部函数最大栈帧约 240 字节系在当前所测宿主环境
#    (macOS arm64 / Apple Clang) 下的静态测量值，而非全平台通用常量。
test-asan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -g"
	@echo "--- 运行 ASan 受限栈回归核验 (8MB 与 1MB 栈) ---"
	@(ulimit -s 8192 && ./$(TEST_CORE_BIN) >/dev/null)
	@echo "  [PASS] ASan 二进制在 ulimit -s 8192 (8MB 栈) 下直接运行正常退出 (rc=0)"
	@(ulimit -s 1024 && ./$(TEST_CORE_BIN) >/dev/null)
	@echo "  [PASS] ASan 二进制在 ulimit -s 1024 (1MB 栈) 下直接运行正常退出 (rc=0)"

clean:
	rm -rf $(TEST_EFW_BIN) $(TEST_CORE_BIN) tests/test_runner* tests/*.dSYM *.dSYM *.efw tests/*.efw
