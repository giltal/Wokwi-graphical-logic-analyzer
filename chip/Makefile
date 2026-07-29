# SPDX-License-Identifier: MIT
# Build the logic-scope Wokwi custom chip (Linux/macOS/CI).
# Override the toolchain location with:  make WASI_SDK_PATH=/opt/wasi-sdk
# Inside Wokwi's builder image (wokwi/builder-clang-wasm) use:
#   make CLANG=clang SYSROOT=/opt/wasi-libc TARGET_TRIPLE=wasm32-unknown-wasi

WASI_SDK_PATH ?= /opt/wasi-sdk
CLANG         ?= $(WASI_SDK_PATH)/bin/clang
SYSROOT       ?= $(WASI_SDK_PATH)/share/wasi-sysroot
TARGET_TRIPLE ?= wasm32-wasip1

SOURCES = $(shell find src -name '*.c')
TARGET  = dist/logic-scope.chip.wasm

# wasi-sdk >= 25 deprecates 'wasm32-unknown-wasi' in favour of 'wasm32-wasip1'
# (same ABI: wasi_snapshot_preview1).
# -Wno-unused-function: wokwi-api.h is vendored verbatim and declares static
# helpers we don't always call.
# stack-size: the simulator imports a memory of only 2 pages (128 KB) and grows
# it on demand, so the declared initial memory (stack + static data) must stay
# under that. Big buffers are malloc'd instead.
CFLAGS  = --target=$(TARGET_TRIPLE) --sysroot=$(SYSROOT) \
          -nostartfiles -Wl,--import-memory -Wl,--export-table -Wl,--no-entry \
          -Wl,-z,stack-size=32768 \
          -Wall -Wextra -Wno-unused-function -Werror -Os

.PHONY: all clean amalgam
all: $(TARGET) dist/logic-scope.chip.json dist/logic-scope.chip.c

dist:
	mkdir -p dist

$(TARGET): dist $(SOURCES) src/wokwi-api.h
	$(CLANG) $(CFLAGS) -o $(TARGET) $(SOURCES)

dist/logic-scope.chip.json: dist logic-scope.chip.json
	cp logic-scope.chip.json dist/

# Single-file source for wokwi.com, which compiles one C file per custom chip.
dist/logic-scope.chip.c: dist $(SOURCES) tools/amalgamate.js
	node tools/amalgamate.js

amalgam: dist/logic-scope.chip.c

clean:
	rm -rf dist
