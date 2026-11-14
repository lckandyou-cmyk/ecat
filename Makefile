CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -Itests/stubs -O2
PYTHON ?= python3

# 阶段 A: efw 容器解析测试源码
SRC_EFW = src/ecat_foe_efw.c
TEST_EFW_SRC = tests/test_efw.c

# 阶段 B: 核心升级状态机、目标驱动与 SOES 适配测试源码
SRC_CORE = src/ecat_foe_efw.c src/ecat_foe_core.c src/adapters/ecat_foe_soes.c
TEST_CORE_SRC = tests/mock_driver.c tests/test_core.c

# 阶段 C: 真实 SOES 源码宿主独立 Harness 测试源码 (按需检测外部 SOES 源码)
TEST_SOES_HARNESS_SRC = tests/test_soes_real_harness.c
SOES_CFLAGS = $(filter-out -Itests/stubs,$(CFLAGS)) -Wno-zero-length-array -Wno-c11-extensions -Wno-strict-prototypes -Itests/soes_bsp -I$(SOES_DIR)

# 阶段 D: 宿主机参考示例与驱动模板源码
EXAMPLES_HOST_DEMO_SRC = examples/host_demo/main.c src/ecat_foe_efw.c src/ecat_foe_core.c
EXAMPLES_DRIVER_TEMPLATE_SRC = examples/driver_template/ecat_foe_driver_template.c
EXAMPLES_BOOTLOADER_SRCS = \
	examples/bootloader_soes/bootloader_flash_driver.c \
	examples/bootloader_soes/bootloader_app.c \
	examples/bootloader_soes/bootloader_master_flow.c
EXAMPLES_BOOTLOADER_ALL_SRC = \
	$(EXAMPLES_BOOTLOADER_SRCS) \
	examples/bootloader_soes/test_bootloader_integration.c

# 阶段 E: Beckhoff SSC 适配层抽象契约与 Mock 测试源码 (契约测试桩，非真实 SSC 验证)
SRC_SSC_ADAPTER = src/ecat_foe_efw.c src/ecat_foe_core.c src/adapters/ecat_foe_ssc.c src/adapters/ecat_foe_ssc_async.c
TEST_SSC_SRC = tests/mock_driver.c tests/test_ssc_adapter_mock.c

BUILD_DIR ?= build
OVERWRITE ?= 0
SOES_DIR ?= /tmp/soes_src/soes
SOES_HASH_STRICT ?= 0
MONOCYPHER_DIR ?=
PDF_SOURCES_DIR ?= $(HOME)/ecat-sources
PDF_FILE ?= $(PDF_SOURCES_DIR)/00001909A.pdf

.PHONY: all build clean test test-asan test-soes test-monocypher test-examples test-ssc-async test-target-async test-core-async test-sentinels vectors check-quotes check-pdf-quotes gen-pdf-table

all: test

# 可选本地构建目标：生成调试二进制至 $(BUILD_DIR)/ 并写入归属证明清单
build:
	@mkdir -p $(BUILD_DIR)
	@if [ "$(OVERWRITE)" = "1" ]; then \
		$(PYTHON) tools/safe_build.py check --build-dir $(BUILD_DIR) --files test_runner_efw test_runner_core --overwrite; \
	else \
		$(PYTHON) tools/safe_build.py check --build-dir $(BUILD_DIR) --files test_runner_efw test_runner_core; \
	fi
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/test_runner_efw $(SRC_EFW) $(TEST_EFW_SRC)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/test_runner_core $(SRC_CORE) $(TEST_CORE_SRC)
	@$(PYTHON) tools/safe_build.py record --build-dir $(BUILD_DIR) --files test_runner_efw test_runner_core
	@echo "  [BUILD] 本地调试二进制已生成至 $(BUILD_DIR)/ 并完成归属登记"

vectors:
	@if [ "$(OVERWRITE)" = "1" ]; then \
		$(PYTHON) tests/generate_vectors.py --overwrite; \
	else \
		$(PYTHON) tests/generate_vectors.py; \
	fi

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

test: test-examples test-ssc-async test-target-async test-core-async
	@which pdftotext >/dev/null 2>&1 || (echo "错误: pdftotext 缺失，请安装 poppler-utils (macOS: brew install poppler)" && exit 1)
	@TMP_BUILD_DIR=$$(mktemp -d /tmp/foe_build_test_XXXXXX) && \
	trap 'rm -rf "$$TMP_BUILD_DIR"' EXIT INT TERM && \
	echo "--- 在独立隔离构建目录运行核心测试套件 (构建目录: $$TMP_BUILD_DIR) ---" && \
	mkdir -p "$$TMP_BUILD_DIR/vectors" && \
	VECTORS_DIR="$$TMP_BUILD_DIR/vectors" $(PYTHON) tests/generate_vectors.py --out-dir "$$TMP_BUILD_DIR/vectors" && \
	$(CC) $(CFLAGS) -o "$$TMP_BUILD_DIR/test_runner_efw" $(SRC_EFW) $(TEST_EFW_SRC) && \
	$(CC) $(CFLAGS) -o "$$TMP_BUILD_DIR/test_runner_core" $(SRC_CORE) $(TEST_CORE_SRC) && \
	$(CC) $(CFLAGS) -o "$$TMP_BUILD_DIR/test_runner_ssc_mock" $(SRC_SSC_ADAPTER) $(TEST_SSC_SRC) && \
	VECTORS_DIR="$$TMP_BUILD_DIR/vectors" "$$TMP_BUILD_DIR/test_runner_efw" && \
	$(PYTHON) tests/test_efw_pack.py && \
	$(PYTHON) tests/test_check_soes_hash.py && \
	$(PYTHON) tests/test_review_docs.py && \
	CC="$(CC)" $(PYTHON) tests/test_config_override.py && \
	CC="$(CC)" $(PYTHON) tests/test_bootloader_assertions.py && \
	"$$TMP_BUILD_DIR/test_runner_core" && \
	echo "--- 运行 Beckhoff SSC 适配层抽象契约测试 (Mock 环境) ---" && \
	"$$TMP_BUILD_DIR/test_runner_ssc_mock" && \
	echo "--- 运行普通测试二进制受限栈回归核验 (8MB 与 1MB 栈) ---" && \
	(ulimit -s 8192 && "$$TMP_BUILD_DIR/test_runner_core" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_BUILD_DIR/test_runner_core" >/dev/null) && \
	echo "  [PASS] 普通 test_runner_core 在 8MB 与 1MB 栈下运行正常" && \
	(ulimit -s 8192 && "$$TMP_BUILD_DIR/test_runner_ssc_mock" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_BUILD_DIR/test_runner_ssc_mock" >/dev/null) && \
	echo "  [PASS] 普通 test_runner_ssc_mock 在 8MB 与 1MB 栈下运行正常" && \
	(ulimit -s 8192 && VECTORS_DIR="$$TMP_BUILD_DIR/vectors" "$$TMP_BUILD_DIR/test_runner_efw" >/dev/null) && \
	(ulimit -s 1024 && VECTORS_DIR="$$TMP_BUILD_DIR/vectors" "$$TMP_BUILD_DIR/test_runner_efw" >/dev/null) && \
	echo "  [PASS] 普通 test_runner_efw 在 8MB 与 1MB 栈下运行正常" && \
	$(PYTHON) tests/test_verify_quotes.py && \
	$(PYTHON) tests/test_verify_pdf_quotes.py

# Optional upstream dependency; CI downloads and verifies the pinned release.
test-monocypher:
	@if [ -z "$(MONOCYPHER_DIR)" ] || [ ! -f "$(MONOCYPHER_DIR)/monocypher.c" ] || [ ! -f "$(MONOCYPHER_DIR)/monocypher.h" ] || [ ! -f "$(MONOCYPHER_DIR)/optional/monocypher-ed25519.c" ] || [ ! -f "$(MONOCYPHER_DIR)/optional/monocypher-ed25519.h" ]; then \
		echo "错误: 请通过 MONOCYPHER_DIR 指定仓库外 Monocypher 4.0.3 的 src 目录"; exit 1; \
	fi
	@TMP_MONOCYPHER_DIR=$$(mktemp -d /tmp/ecat_monocypher_test_XXXXXX) && \
	trap 'rm -rf "$$TMP_MONOCYPHER_DIR"' EXIT INT TERM && \
	$(CC) $(CFLAGS) -Iexamples/crypto -I"$(MONOCYPHER_DIR)" -I"$(MONOCYPHER_DIR)/optional" \
		-o "$$TMP_MONOCYPHER_DIR/interop" src/ecat_foe_efw.c src/ecat_foe_core.c \
		examples/crypto/ecat_foe_monocypher_verifier.c tests/mock_driver.c tests/test_monocypher_interop.c \
		"$(MONOCYPHER_DIR)/monocypher.c" "$(MONOCYPHER_DIR)/optional/monocypher-ed25519.c" && \
	$(PYTHON) tests/test_monocypher_interop.py "$$TMP_MONOCYPHER_DIR/interop"

test-ssc-async:
	@TMP_ASYNC_DIR=$$(mktemp -d /tmp/foe_ssc_async_XXXXXX) && \
	trap 'rm -rf "$$TMP_ASYNC_DIR"' EXIT INT TERM && \
	$(CC) $(CFLAGS) -o "$$TMP_ASYNC_DIR/test_ssc_async_gate" \
		src/adapters/ecat_foe_ssc_async.c tests/test_ssc_async_gate.c && \
	"$$TMP_ASYNC_DIR/test_ssc_async_gate" && \
	$(CC) $(CFLAGS) -o "$$TMP_ASYNC_DIR/test_ssc_async_adapter" \
		$(SRC_SSC_ADAPTER) tests/mock_driver.c tests/test_ssc_async_adapter.c && \
	"$$TMP_ASYNC_DIR/test_ssc_async_adapter"

test-target-async:
	@TMP_ASYNC_DIR=$$(mktemp -d /tmp/foe_target_async_XXXXXX) && \
	trap 'rm -rf "$$TMP_ASYNC_DIR"' EXIT INT TERM && \
	$(CC) $(CFLAGS) -o "$$TMP_ASYNC_DIR/test_target_async" \
		src/ecat_foe_target_async.c tests/test_target_async.c && \
	"$$TMP_ASYNC_DIR/test_target_async"

test-core-async:
	@TMP_ASYNC_DIR=$$(mktemp -d /tmp/foe_core_async_XXXXXX) && \
	trap 'rm -rf "$$TMP_ASYNC_DIR"' EXIT INT TERM && \
	$(CC) $(CFLAGS) -o "$$TMP_ASYNC_DIR/test_core_async" \
		src/ecat_foe_efw.c src/ecat_foe_core.c src/ecat_foe_target_async.c \
		src/ecat_foe_core_async.c src/adapters/ecat_foe_ssc_async.c \
		src/adapters/ecat_foe_ssc.c tests/mock_driver.c tests/test_core_async.c && \
	"$$TMP_ASYNC_DIR/test_core_async"

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
	echo "--- 静态编译检查驱动模板与 Bootloader 接入参考 (在独立临时构建目录) ---" && \
	$(CC) $(CFLAGS) -c -o "$$TMP_BUILD_DIR/driver_template.o" $(EXAMPLES_DRIVER_TEMPLATE_SRC) && \
	$(CC) $(CFLAGS) -Iexamples/driver_template -o "$$TMP_BUILD_DIR/test_driver_template" tests/test_driver_template.c $(EXAMPLES_DRIVER_TEMPLATE_SRC) && \
	"$$TMP_BUILD_DIR/test_driver_template" && \
	$(CC) $(CFLAGS) -Iexamples/bootloader_soes -c -o "$$TMP_BUILD_DIR/bootloader_flash_driver.o" examples/bootloader_soes/bootloader_flash_driver.c && \
	$(CC) $(CFLAGS) -Iexamples/bootloader_soes -c -o "$$TMP_BUILD_DIR/bootloader_app.o" examples/bootloader_soes/bootloader_app.c && \
	$(CC) $(CFLAGS) -Iexamples/bootloader_soes -c -o "$$TMP_BUILD_DIR/bootloader_master_flow.o" examples/bootloader_soes/bootloader_master_flow.c && \
	echo "  [PASS] 驱动模板与 Bootloader 接入参考静态编译检查通过 (-Wall -Wextra -Werror -pedantic)"

test-bootloader:
	@if [ -z "$(SOES_DIR)" ] || [ ! -f "$(SOES_DIR)/esc_foe.c" ] || [ ! -f "$(SOES_DIR)/esc.c" ]; then \
		echo "错误: SOES_DIR 缺失或未包含 esc_foe.c / esc.c: '$(SOES_DIR)'"; \
		echo "使用方法: make test-bootloader SOES_DIR=/path/to/soes"; \
		exit 1; \
	fi
	@$(PYTHON) tools/check_soes_hash.py --soes-dir "$(SOES_DIR)" $(if $(filter 1,$(SOES_HASH_STRICT)),--strict,)
	@TMP_SOES_DIR=$$(mktemp -d /tmp/foe_build_bootloader_XXXXXX) && \
	trap 'rm -rf "$$TMP_SOES_DIR"' EXIT INT TERM && \
	echo "--- 在独立隔离构建目录编译并运行最小 Bootloader 集成验证 (构建目录: $$TMP_SOES_DIR) ---" && \
	$(CC) $(SOES_CFLAGS) -Iexamples/bootloader_soes -o "$$TMP_SOES_DIR/test_runner_bootloader_soes" $(SRC_CORE) $(EXAMPLES_BOOTLOADER_ALL_SRC) $(SOES_DIR)/esc_foe.c $(SOES_DIR)/esc.c && \
	"$$TMP_SOES_DIR/test_runner_bootloader_soes" && \
	(ulimit -s 8192 && "$$TMP_SOES_DIR/test_runner_bootloader_soes" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_SOES_DIR/test_runner_bootloader_soes" >/dev/null) && \
	echo "  [PASS] 最小 Bootloader 集成测试在 8MB 与 1MB 栈下运行正常"

test-soes:
	@if [ -z "$(SOES_DIR)" ] || [ ! -f "$(SOES_DIR)/esc_foe.c" ] || [ ! -f "$(SOES_DIR)/esc.c" ]; then \
		echo "错误: SOES_DIR 缺失或未包含 esc_foe.c / esc.c: '$(SOES_DIR)'"; \
		echo "使用方法: make test-soes SOES_DIR=/path/to/soes"; \
		exit 1; \
	fi
	@$(PYTHON) tools/check_soes_hash.py --soes-dir "$(SOES_DIR)" $(if $(filter 1,$(SOES_HASH_STRICT)),--strict,)
	@TMP_SOES_DIR=$$(mktemp -d /tmp/foe_build_soes_XXXXXX) && \
	trap 'rm -rf "$$TMP_SOES_DIR"' EXIT INT TERM && \
	echo "--- 在独立隔离构建目录编译并运行真实 SOES Harness (构建目录: $$TMP_SOES_DIR) ---" && \
	$(CC) $(SOES_CFLAGS) -o "$$TMP_SOES_DIR/test_runner_soes_real" $(SRC_CORE) $(TEST_SOES_HARNESS_SRC) tests/mock_driver.c $(SOES_DIR)/esc_foe.c $(SOES_DIR)/esc.c && \
	"$$TMP_SOES_DIR/test_runner_soes_real" && \
	(ulimit -s 8192 && "$$TMP_SOES_DIR/test_runner_soes_real" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_SOES_DIR/test_runner_soes_real" >/dev/null) && \
	echo "  [PASS] 真实 SOES Harness 在 8MB 与 1MB 栈下运行正常" && \
	echo "--- 在独立隔离构建目录编译并运行最小 Bootloader 集成验证 (构建目录: $$TMP_SOES_DIR) ---" && \
	$(CC) $(SOES_CFLAGS) -Iexamples/bootloader_soes -o "$$TMP_SOES_DIR/test_runner_bootloader_soes" $(SRC_CORE) $(EXAMPLES_BOOTLOADER_ALL_SRC) $(SOES_DIR)/esc_foe.c $(SOES_DIR)/esc.c && \
	"$$TMP_SOES_DIR/test_runner_bootloader_soes" && \
	(ulimit -s 8192 && "$$TMP_SOES_DIR/test_runner_bootloader_soes" >/dev/null) && \
	(ulimit -s 1024 && "$$TMP_SOES_DIR/test_runner_bootloader_soes" >/dev/null) && \
	echo "  [PASS] 最小 Bootloader 集成测试在 8MB 与 1MB 栈下运行正常"

# ASan 内存与未定义行为核验目标 (完整运行示例、Python 工具与全套 C 二进制测试)
# 历史构建与环境条件复核说明:
# 1. 历史复核事实 (commit 9c78bc2 与 c1792c4):
#    - 在 macOS 下通过 GNU Make 执行 `make test-asan` 时，Make 为子进程赋予了较大栈上限 (实测为 65520KB)，测试退出码为 0；
#    - 但生成的同一 ASan 二进制脱离 make 时，在 commit c1792c4 实测：以 ulimit -s 8192 (8MB) 或 1024 (1MB) 独立执行，
#      均因多个测试用例将 1MB 模拟缓冲区分配于栈上且被 Clang -O2 内联至 main()，导致 stack-overflow 退出码 134，堆栈指向 main 入口。
# 2. 夹具修复与回归验证 (commit e31190d 起):
#    已将 tests/test_core.c 内测试夹具统一调整为静态存储 (static/BSS)。
#    回归核验：同一 ASan 二进制在 ulimit -s 8192 与 1024 下独立执行均顺利通过 (退出码 0)。
# 3. 栈帧尺寸限定：核心库代码 (src/ecat_foe_core.c) 为零动态分配，内部函数最大栈帧约 240 字节系在当前所测宿主环境
#    (macOS arm64 / Apple Clang) 下的静态测量值，而非通用常量。
test-asan:
	@which pdftotext >/dev/null 2>&1 || (echo "错误: pdftotext 缺失，请安装 poppler-utils (macOS: brew install poppler)" && exit 1)
	@echo "--- 编译并运行宿主示例套件 (ASan/UBSan 模式) ---"
	@$(MAKE) test-examples CC="$(CC)" CFLAGS="$(CFLAGS) -fsanitize=address,undefined -g"
	@TMP_ASAN_DIR=$$(mktemp -d /tmp/foe_build_asan_XXXXXX) && \
	trap 'rm -rf "$$TMP_ASAN_DIR"' EXIT INT TERM && \
	echo "--- 在独立隔离构建目录运行 ASan/UBSan 核心测试套件 (构建目录: $$TMP_ASAN_DIR) ---" && \
	mkdir -p "$$TMP_ASAN_DIR/vectors" && \
	VECTORS_DIR="$$TMP_ASAN_DIR/vectors" $(PYTHON) tests/generate_vectors.py --out-dir "$$TMP_ASAN_DIR/vectors" && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_runner_efw" $(SRC_EFW) $(TEST_EFW_SRC) && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_runner_core" $(SRC_CORE) $(TEST_CORE_SRC) && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_runner_ssc_mock" $(SRC_SSC_ADAPTER) $(TEST_SSC_SRC) && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_ssc_async_gate" src/adapters/ecat_foe_ssc_async.c tests/test_ssc_async_gate.c && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_ssc_async_adapter" $(SRC_SSC_ADAPTER) tests/mock_driver.c tests/test_ssc_async_adapter.c && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_target_async" src/ecat_foe_target_async.c tests/test_target_async.c && \
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o "$$TMP_ASAN_DIR/test_core_async" src/ecat_foe_efw.c src/ecat_foe_core.c src/ecat_foe_target_async.c src/ecat_foe_core_async.c src/adapters/ecat_foe_ssc_async.c src/adapters/ecat_foe_ssc.c tests/mock_driver.c tests/test_core_async.c && \
	VECTORS_DIR="$$TMP_ASAN_DIR/vectors" "$$TMP_ASAN_DIR/test_runner_efw" && \
	$(PYTHON) tests/test_efw_pack.py && \
	"$$TMP_ASAN_DIR/test_runner_core" && \
	echo "--- 运行 Beckhoff SSC 适配层抽象契约测试 (ASan/UBSan Mock 环境) ---" && \
	"$$TMP_ASAN_DIR/test_runner_ssc_mock" && \
	"$$TMP_ASAN_DIR/test_ssc_async_gate" && \
	"$$TMP_ASAN_DIR/test_ssc_async_adapter" && \
	"$$TMP_ASAN_DIR/test_target_async" && \
	"$$TMP_ASAN_DIR/test_core_async" && \
	echo "--- 运行 ASan 受限栈回归核验 (8MB 与 1MB 栈) ---" && \
	(ulimit -s 8192 && "$$TMP_ASAN_DIR/test_runner_core" >/dev/null) && \
	echo "  [PASS] ASan 二进制在 ulimit -s 8192 (8MB 栈) 下直接运行正常退出 (rc=0)" && \
	(ulimit -s 1024 && "$$TMP_ASAN_DIR/test_runner_core" >/dev/null) && \
	echo "  [PASS] ASan 二进制在 ulimit -s 1024 (1MB 栈) 下直接运行正常退出 (rc=0)" && \
	(ulimit -s 8192 && "$$TMP_ASAN_DIR/test_runner_ssc_mock" >/dev/null) && \
	echo "  [PASS] ASan test_runner_ssc_mock 在 8MB 栈下运行正常" && \
	(ulimit -s 1024 && "$$TMP_ASAN_DIR/test_runner_ssc_mock" >/dev/null) && \
	echo "  [PASS] ASan test_runner_ssc_mock 在 1MB 栈下运行正常" && \
	(ulimit -s 8192 && VECTORS_DIR="$$TMP_ASAN_DIR/vectors" "$$TMP_ASAN_DIR/test_runner_efw" >/dev/null) && \
	(ulimit -s 1024 && VECTORS_DIR="$$TMP_ASAN_DIR/vectors" "$$TMP_ASAN_DIR/test_runner_efw" >/dev/null) && \
	echo "  [PASS] ASan test_runner_efw 在 8MB 与 1MB 栈下运行正常" && \
	$(PYTHON) tests/test_verify_quotes.py && \
	$(PYTHON) tests/test_verify_pdf_quotes.py

clean:
	@echo "--- 执行安全清理 (安全模式: 仅清理明确由构建生成的隔离产物，不匹配或删除用户文件) ---"
	@if [ -d "$(BUILD_DIR)" ]; then \
		$(PYTHON) tools/safe_build.py clean --build-dir "$(BUILD_DIR)"; \
	fi
