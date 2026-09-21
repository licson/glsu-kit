# Build the glsu-kit device binaries with the Android NDK toolchain.
# Usage: ANDROID_NDK_HOME=/path/to/android-ndk make
NDK_HOME ?= $(ANDROID_NDK_HOME)
ifeq ($(NDK_HOME),)
$(error set ANDROID_NDK_HOME (or NDK_HOME) to your Android NDK directory)
endif

PREBUILT := $(firstword $(wildcard $(NDK_HOME)/toolchains/llvm/prebuilt/darwin-arm64 \
                              $(NDK_HOME)/toolchains/llvm/prebuilt/darwin-x86_64 \
                              $(NDK_HOME)/toolchains/llvm/prebuilt/linux-x86_64))
CC := $(PREBUILT)/bin/aarch64-linux-android35-clang
CFLAGS ?= -O2 -Wall
BIN := bin

all: $(BIN)/glsu $(BIN)/diagtty $(BIN)/qrtr_probe $(BIN)/diag_send

$(BIN)/%: src/%.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)

.PHONY: all clean
