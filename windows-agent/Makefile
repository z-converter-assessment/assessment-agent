# ============================================================
# assessment-agent (Windows) — MinGW-w64 build
#
# Build host expectation: MSYS2 + mingw-w64 toolchain on Windows.
# `gcc`/`ar`/`cmake` in PATH resolve to the mingw64 toolchain.
#
# Usage:
#   make vendor-fetch   # one-time: git clone vendor deps
#   make vendor-build   # configure + compile static libs (cmake required)
#   make                # build assessment-agent.exe (statically linked)
#   make verify         # confirm only Windows system DLLs are referenced
#   make release        # build + verify + dist/ packaging with SHA256SUMS
#
# Cross-compile (Linux/macOS host with x86_64-w64-mingw32 toolchain):
#   make CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar \
#        OPENSSL_TARGET=mingw64 OPENSSL_CROSS=--cross-compile-prefix=x86_64-w64-mingw32-
# ============================================================

CC      ?= gcc
AR      ?= ar
CMAKE   ?= cmake
JOBS    ?= 4

CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11 \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
LDFLAGS ?= -static -static-libgcc -Wl,--no-insert-timestamp

AGENT_VERSION ?= 1.0.0

# ----------------------------------------------------------------------
# Vendor static libraries (gitignored — built locally before agent build)
# ----------------------------------------------------------------------
VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION := v0.15.0
OPENSSL_VERSION    := openssl-3.0.15
ZLIB_VERSION       := v1.3.1

CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
OPENSSL_DIR        := $(VENDOR_DIR)/openssl
ZLIB_DIR           := $(VENDOR_DIR)/zlib

CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
OPENSSL_SSL        := $(OPENSSL_DIR)/install/lib/libssl.a
OPENSSL_CRYPTO     := $(OPENSSL_DIR)/install/lib/libcrypto.a
ZLIB_LIB           := $(ZLIB_DIR)/libz.a

OPENSSL_TARGET     ?= mingw64
OPENSSL_CROSS      ?=

CFLAGS += -I$(CJSON_DIR) \
          -I$(RABBITMQ_C_DIR)/include \
          -I$(RABBITMQ_C_DIR)/build/include \
          -I$(OPENSSL_DIR)/install/include \
          -I$(ZLIB_DIR) \
          -Iinclude \
          -DAGENT_VERSION=\"$(AGENT_VERSION)\"

# Static vendor archives + Windows system DLLs (kept dynamic — they ship
# with every supported Windows version, see deploy/SUPPORTED_OS.md):
#   ws2_32   : Winsock (AMQP socket, gethostname, inet_ntop)
#   iphlpapi : GetAdaptersAddresses, GetIfTable2 (network inventory + io)
#   advapi32 : Registry, Service Control Manager, EnumServicesStatusEx
#   crypt32  : OpenSSL on Windows needs CertOpenSystemStoreW etc.
#   secur32  : SSPI (used transitively by some Schannel paths)
#   bcrypt   : BCryptGenRandom (UUID v4 generation)
#   ole32 / oleaut32 : COM init (used by some Windows API surfaces)
LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
          -lws2_32 -liphlpapi -ladvapi32 -lkernel32 -luser32 \
          -lcrypt32 -lsecur32 -lbcrypt -lole32 -loleaut32

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := assessment-agent.exe

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ----------------------------------------------------------------------
# Vendor fetch (one-time clone — network required)
# ----------------------------------------------------------------------
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git  $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git $(RABBITMQ_C_DIR)
	@test -d $(OPENSSL_DIR)    || git clone --depth 1 --branch $(OPENSSL_VERSION)    https://github.com/openssl/openssl.git   $(OPENSSL_DIR)
	@test -d $(ZLIB_DIR)       || git clone --depth 1 --branch $(ZLIB_VERSION)       https://github.com/madler/zlib.git       $(ZLIB_DIR)

# ----------------------------------------------------------------------
# Vendor build — MinGW64 static
# ----------------------------------------------------------------------
vendor-build: vendor-fetch vendor-build-openssl vendor-build-zlib vendor-build-cjson vendor-build-rabbitmq

vendor-build-openssl:
	# `--libdir=lib` 명시 — OpenSSL 3.x 가 host triplet 에 따라 `lib64` 또는
	# `lib/<host>` 로 install 하면 Makefile 의 $(OPENSSL_SSL) 고정 경로
	# (`install/lib/libssl.a`) 와 어긋난다.
	cd $(OPENSSL_DIR) && ./Configure $(OPENSSL_TARGET) no-shared no-dso no-engine no-tests $(OPENSSL_CROSS) \
	    --libdir=lib --prefix=$(CURDIR)/$(OPENSSL_DIR)/install
	$(MAKE) -C $(OPENSSL_DIR) -j$(JOBS)
	$(MAKE) -C $(OPENSSL_DIR) install_sw

vendor-build-zlib:
	# zlib win32/Makefile.gcc 가 ARFLAGS=rcs 를 자체 정의함. AR 변수에는
	# 명령만 전달 (옵션 X) — 그렇지 않으면 `ar rc rcs libz.a ...` 으로
	# rcs 가 파일명으로 해석되어 실패.
	$(MAKE) -C $(ZLIB_DIR) -f win32/Makefile.gcc CC=$(CC) AR=$(AR) libz.a

vendor-build-cjson:
	$(CMAKE) -S $(CJSON_DIR) -B $(CJSON_DIR)/build \
	      -DCMAKE_C_COMPILER=$(CC) \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(CJSON_DIR)/build --target cjson -- -j$(JOBS)

vendor-build-rabbitmq:
	# GCC 14+ (MSYS2 mingw64 최신) 가 -Wincompatible-pointer-types 를 error 로
	# promote — rabbitmq-c v0.15.0 amqp_socket.c:309 의 ioctlsocket(int*) ↔
	# Windows winsock2.h 의 u_long* 미스매치를 거부. upstream fix 전까지 demote.
	$(CMAKE) -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build \
	      -DCMAKE_C_COMPILER=$(CC) \
	      -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types" \
	      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	      -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
	      -DENABLE_SSL_SUPPORT=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static -- -j$(JOBS)
	# Windows mingw cmake 가 정적 라이브러리에 `lib` prefix 를 자동 추가하면서
	# SOVERSION 까지 붙여 `liblibrabbitmq.<N>.a` 형태로 출력한다. Linux 와 동일
	# 경로 (`librabbitmq.a`) 로 별칭을 만들어 $(RABBITMQ_C_LIB) 경로를 통일.
	cp $(RABBITMQ_C_DIR)/build/librabbitmq/liblibrabbitmq.*.a \
	   $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(BIN) build-deps.txt

# ----------------------------------------------------------------------
# Verify — only Windows system DLLs allowed in import table
# ----------------------------------------------------------------------
verify: $(BIN)
	@echo "[verify] checking DLL dependencies of $(BIN)..."
	@objdump -p $(BIN) | awk '/DLL Name:/ {print tolower($$3)}' | sort -u > build-deps.txt
	@cat build-deps.txt
	@if grep -vE '^(kernel32|advapi32|user32|ws2_32|iphlpapi|crypt32|secur32|bcrypt|ole32|oleaut32|msvcrt|ntdll|api-ms-win-.+)\.dll$$' build-deps.txt; then \
		echo "[verify] FAIL: non-system DLL dependency listed above"; \
		exit 1; \
	fi
	@echo "[verify] OK: only Windows system DLLs referenced"

# ----------------------------------------------------------------------
# Release packaging — dist/assessment-agent.exe + SHA256SUMS
# ----------------------------------------------------------------------
DIST_DIR := dist

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_DIR)/
	cd $(DIST_DIR) && sha256sum $(BIN) > SHA256SUMS
	@echo "[release] packaged $(DIST_DIR)/$(BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean vendor-fetch vendor-build vendor-build-openssl vendor-build-zlib \
        vendor-build-cjson vendor-build-rabbitmq vendor-clean verify release
