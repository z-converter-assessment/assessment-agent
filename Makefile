CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?=

# AGENT_VERSION can be overridden at build time:
#   make AGENT_VERSION=1.2.3
AGENT_VERSION ?= 1.0.0

# ----------------------------------------------------------------------
# Library resolution
#   default      : pkg-config (system-installed libs) — dev / unit-test path
#   USE_VENDORED : link statically against vendor/{...} — release path
#
# Release (USE_VENDORED=1) static set: cJSON, rabbitmq-c, libcurl, libarchive,
# OpenSSL, zlib. Only glibc-family libs remain dynamic (libc/libpthread/
# libdl/libm/libresolv/librt). `make verify` enforces this whitelist.
# ----------------------------------------------------------------------
PKGS               := librabbitmq libcjson libcurl libarchive

VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION := v0.15.0
CURL_VERSION       := curl-8_10_1
LIBARCHIVE_VERSION := v3.7.7
OPENSSL_VERSION    := openssl-3.0.15
ZLIB_VERSION       := v1.3.1

CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CURL_DIR           := $(VENDOR_DIR)/curl
LIBARCHIVE_DIR     := $(VENDOR_DIR)/libarchive
OPENSSL_DIR        := $(VENDOR_DIR)/openssl
ZLIB_DIR           := $(VENDOR_DIR)/zlib

CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
CURL_LIB           := $(CURL_DIR)/build/lib/libcurl.a
LIBARCHIVE_LIB     := $(LIBARCHIVE_DIR)/build/libarchive/libarchive.a
OPENSSL_SSL        := $(OPENSSL_DIR)/install/lib/libssl.a
OPENSSL_CRYPTO     := $(OPENSSL_DIR)/install/lib/libcrypto.a
ZLIB_LIB           := $(ZLIB_DIR)/libz.a

ifeq ($(USE_VENDORED),1)
  CFLAGS  += -I$(CJSON_DIR) \
             -I$(RABBITMQ_C_DIR)/librabbitmq \
             -I$(RABBITMQ_C_DIR)/build/librabbitmq \
             -I$(CURL_DIR)/include \
             -I$(LIBARCHIVE_DIR)/libarchive \
             -I$(OPENSSL_DIR)/install/include \
             -I$(ZLIB_DIR)
  LDLIBS  := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(LIBARCHIVE_LIB) \
             $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
             -lpthread -ldl
  # Hardening + size: PIE, full RELRO, BIND_NOW, drop unused libs, static libgcc.
  # `make verify` confirms only the glibc whitelist remains in ldd output.
  LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,--as-needed -static-libgcc
else
  CFLAGS += $(shell pkg-config --cflags $(PKGS))
  LDLIBS := $(shell pkg-config --libs $(PKGS))
endif

CFLAGS  += -Iinclude -DAGENT_VERSION=\"$(AGENT_VERSION)\"

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := assessment-agent

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ----------------------------------------------------------------------
# Unit tests (parser/helper level — no broker, no network)
#   make test-unit
# Tests use pkg-config dev libs regardless of USE_VENDORED — they exercise
# parsing logic, not the static-link surface.
# ----------------------------------------------------------------------
TEST_BIN    := tests/unit/run_unit
TEST_CFLAGS := -Wall -Wextra -Wpedantic -Wno-unused-function -O0 -g -std=c11 \
               -Iinclude -DAGENT_VERSION=\"test\" \
               $(shell pkg-config --cflags libcjson libcurl libarchive)
TEST_LDLIBS := $(shell pkg-config --libs libcjson libcurl libarchive) -lcrypto

$(TEST_BIN): tests/unit/test_main.c tests/unit/tinytest.h \
             src/util.c src/collect.c src/download.c src/extract.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/unit/test_main.c src/util.c $(TEST_LDLIBS)

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

# ----------------------------------------------------------------------
# Vendoring — fetches deps into vendor/, builds static libs.
#   make vendor-fetch         # one-time clone (git, network required)
#   make vendor-build         # configure + compile static libs (cmake required)
#   make USE_VENDORED=1       # build the agent against vendored static libs
#
# Two ways to satisfy build-host prerequisites — DO NOT install them manually:
#   (1) Containerized (recommended, host needs only Docker):
#         ./scripts/build-linux.sh        # one shot, no host-side setup
#   (2) Native amd64 Linux build host:
#         sudo bash scripts/build-prep.sh # auto-detects apt or yum/dnf
#         make vendor-fetch && make vendor-build && make USE_VENDORED=1 release
#
# Both paths install the same set: gcc, make, cmake, git, pkgconfig, perl,
# perl-IPC-Cmd (OpenSSL 3.0+ Configure requirement). The wrapper scripts
# handle the apt/yum split.
#
# Note: release artifacts must come from **native amd64 Linux**. ARM Macs
# can run the container under Rosetta/QEMU emulation for development but
# the resulting emulated binary should not be trusted as a release until
# reproduced on native amd64 (CI / build host).
#
# Build order matters: OpenSSL + zlib first (curl/libarchive/rabbitmq-c link
# against them). cmake projects are pointed at the vendored OpenSSL via
# OPENSSL_ROOT_DIR + OPENSSL_USE_STATIC_LIBS so they don't pick up the host
# distro's libssl/libcrypto (would defeat ABI portability).
# ----------------------------------------------------------------------
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git       $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git      $(RABBITMQ_C_DIR)
	@test -d $(CURL_DIR)       || git clone --depth 1 --branch $(CURL_VERSION)       https://github.com/curl/curl.git              $(CURL_DIR)
	@test -d $(LIBARCHIVE_DIR) || git clone --depth 1 --branch $(LIBARCHIVE_VERSION) https://github.com/libarchive/libarchive.git  $(LIBARCHIVE_DIR)
	@test -d $(OPENSSL_DIR)    || git clone --depth 1 --branch $(OPENSSL_VERSION)    https://github.com/openssl/openssl.git        $(OPENSSL_DIR)
	@test -d $(ZLIB_DIR)       || git clone --depth 1 --branch $(ZLIB_VERSION)       https://github.com/madler/zlib.git            $(ZLIB_DIR)

vendor-build: vendor-fetch \
              vendor-build-openssl vendor-build-zlib \
              vendor-build-cjson   vendor-build-rabbitmq \
              vendor-build-curl    vendor-build-libarchive

vendor-build-openssl:
	cd $(OPENSSL_DIR) && ./Configure linux-x86_64 no-shared no-dso no-engine no-tests \
	    --libdir=lib -fPIC --prefix=$(CURDIR)/$(OPENSSL_DIR)/install
	$(MAKE) -C $(OPENSSL_DIR)
	$(MAKE) -C $(OPENSSL_DIR) install_sw

vendor-build-zlib:
	cd $(ZLIB_DIR) && CFLAGS=-fPIC ./configure --static
	$(MAKE) -C $(ZLIB_DIR) libz.a

vendor-build-cjson:
	cmake -S $(CJSON_DIR) -B $(CJSON_DIR)/build \
	      -DCJSON_BUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(CJSON_DIR)/build --target cjson

vendor-build-rabbitmq:
	cmake -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	      -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
	      -DENABLE_SSL_SUPPORT=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static

vendor-build-curl:
	cmake -S $(CURL_DIR) -B $(CURL_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DCURL_STATICLIB=ON \
	      -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
	      -DCURL_USE_OPENSSL=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_FTP=ON \
	      -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
	      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_POP3=ON \
	      -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(CURL_DIR)/build --target libcurl_static

vendor-build-libarchive:
	cmake -S $(LIBARCHIVE_DIR) -B $(LIBARCHIVE_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_TEST=OFF \
	      -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
	      -DENABLE_OPENSSL=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(LIBARCHIVE_DIR)/build --target archive_static

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

# ----------------------------------------------------------------------
# verify — manylinux2014 ABI compliance.
#
# Gates the binary on three things:
#   1. No GLIBC_2.18+ symbols (build container = glibc 2.17 = CentOS 7).
#   2. Only the documented dynamic deps (whitelist below).
#   3. No forbidden API symbols (kernel 3.10 / glibc 2.17 ceiling).
#
# Fail-fast for `make release` (CI calls release, which depends on verify).
# ----------------------------------------------------------------------
ALLOWED_DLLS := linux-vdso libc libpthread libdl libm libresolv librt ld-linux-x86-64

verify: $(BIN)
	@echo "[verify] $(BIN) — manylinux2014 ABI compliance"
	@bad=$$(objdump -T $(BIN) 2>/dev/null | awk '/GLIBC_/ {print $$5}' \
	         | grep -E 'GLIBC_2\.(1[89]|[2-9][0-9])' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: GLIBC 2.18+ symbols found (breaks manylinux2014 ABI):"; echo "$$bad"; exit 1; fi
	@deps=$$(ldd $(BIN) | awk '/=>/ {print $$1} /linux-vdso/ {print $$1}' \
	         | sed -E 's/\.so.*$$//' | sort -u); \
	 for d in $$deps; do \
	   case " $(ALLOWED_DLLS) " in *" $$d "*) ;; \
	     *) echo "[verify] FAIL: unexpected dynamic dep '$$d.so'"; \
	        echo "[verify]   allowed: $(ALLOWED_DLLS)"; exit 1 ;; \
	   esac; \
	 done
	@bad=$$(nm -D $(BIN) 2>/dev/null | grep -E ' U (getrandom|statx|memfd_create|renameat2|copy_file_range|pidfd_)' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: forbidden APIs (glibc 2.17 / kernel 3.10 ceiling):"; echo "$$bad"; exit 1; fi
	@echo "[verify] OK: GLIBC 2.17 clean, dyn-dep whitelist matches, no forbidden APIs"

# ----------------------------------------------------------------------
# release — produce dist/assessment-agent-linux-x86_64 + SHA256SUMS.
#
# Usage (from a manylinux2014 build host):
#   docker run --rm -v $(pwd):/src -w /src quay.io/pypa/manylinux2014_x86_64 \
#       bash -lc 'make vendor-fetch && make vendor-build && make USE_VENDORED=1 release'
#
# install.sh on the target server SHA256-verifies the dist/ artifact.
# ----------------------------------------------------------------------
DIST_DIR := dist
DIST_BIN := $(DIST_DIR)/assessment-agent-linux-x86_64

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_BIN)
	cd $(DIST_DIR) && sha256sum assessment-agent-linux-x86_64 > SHA256SUMS
	@echo "[release] packaged $(DIST_BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean test-unit \
        vendor-fetch vendor-build vendor-clean \
        vendor-build-openssl vendor-build-zlib vendor-build-cjson \
        vendor-build-rabbitmq vendor-build-curl vendor-build-libarchive \
        verify release
