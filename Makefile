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
  # -fPIE for PIE executable (matches LDFLAGS -pie). Vendor static libs are
  # already -fPIC compiled, so the resulting binary is fully position-independent.
  CFLAGS  += -fPIE \
             -I$(CJSON_DIR) \
             -I$(RABBITMQ_C_DIR)/include \
             -I$(RABBITMQ_C_DIR)/build/include \
             -I$(CURL_DIR)/include \
             -I$(LIBARCHIVE_DIR)/libarchive \
             -I$(OPENSSL_DIR)/install/include \
             -I$(ZLIB_DIR)
  LDLIBS  := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(LIBARCHIVE_LIB) \
             $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
             -lpthread -ldl -lrt
  # -lrt: clock_gettime lives in librt on glibc < 2.17 (CentOS 6 / legacy
  # build); folded into libc since 2.17. `-Wl,--as-needed` drops it on modern.
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

# ----------------------------------------------------------------------
# Embedded sh / systemd unit / env example.
#
# `ld -r -b binary` packs each input file as raw bytes and emits
#   _binary_<basename>_start / _end / _size
# symbols (non-alnum chars in the basename become underscores). The
# installer subcommand reads these blobs and dumps them under
# /tmp/agent-installer-XXXXXX/ before exec'ing /bin/sh against install.sh.
#
# We stage the inputs into build/embed/ with flat names so the symbols are
# `_binary_install_sh_start` (etc) rather than `_binary_deploy_install_sh_start`.
# A single ld -r call collapses all blobs into one .o and objcopy moves the
# bytes from .data to .rodata so they cannot be modified at runtime.
# ----------------------------------------------------------------------
LD       ?= ld
OBJCOPY  ?= objcopy
EMBED_DIR := build/embed
EMBED_OBJ := $(EMBED_DIR)/embed.o

$(EMBED_DIR)/install.sh:               deploy/install.sh
$(EMBED_DIR)/uninstall.sh:             deploy/uninstall.sh
$(EMBED_DIR)/image-prep.sh:            scripts/image-prep.sh
$(EMBED_DIR)/detect-os.sh:             deploy/lib/detect-os.sh
$(EMBED_DIR)/env-setup.sh:             deploy/lib/env-setup.sh
$(EMBED_DIR)/assessment-agent.service: deploy/systemd/assessment-agent.service
$(EMBED_DIR)/assessment-agent.sysv:    deploy/sysv/assessment-agent
$(EMBED_DIR)/agent.env.example:        deploy/systemd/agent.env.example

$(EMBED_DIR):
	mkdir -p $@

$(EMBED_DIR)/%: | $(EMBED_DIR)
	cp $< $@

EMBED_STAGED := \
    $(EMBED_DIR)/install.sh \
    $(EMBED_DIR)/uninstall.sh \
    $(EMBED_DIR)/image-prep.sh \
    $(EMBED_DIR)/detect-os.sh \
    $(EMBED_DIR)/env-setup.sh \
    $(EMBED_DIR)/assessment-agent.service \
    $(EMBED_DIR)/assessment-agent.sysv \
    $(EMBED_DIR)/agent.env.example

$(EMBED_OBJ): $(EMBED_STAGED)
	cd $(EMBED_DIR) && $(LD) -r -b binary -o embed.raw.o $(notdir $(EMBED_STAGED))
	$(OBJCOPY) --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(EMBED_DIR)/embed.raw.o $@
	rm -f $(EMBED_DIR)/embed.raw.o

all: $(BIN)

$(BIN): $(OBJ) $(EMBED_OBJ)
	$(CC) $(OBJ) $(EMBED_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

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
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
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
	rm -f $(OBJ) $(BIN) $(LEGACY_BIN) $(TEST_BIN)
	rm -rf $(EMBED_DIR)

# ----------------------------------------------------------------------
# verify — GLIBC / dyn-dep / forbidden-API ABI compliance.
#
# Two profiles, parameterized by the variables below:
#   modern (default) : manylinux2014 / glibc 2.17 / CentOS 7 baseline.
#                      Forbids GLIBC_2.18+ symbols.
#   legacy           : manylinux2010 / glibc 2.12 / CentOS 6 baseline.
#                      Forbids GLIBC_2.13+ symbols.
#
# The verify recipe is a single parameterized body; `verify` (modern) and
# `verify-legacy` set the knobs and the binary under test. The modern path
# is byte-for-byte the same checks as before — only the regex moved into a
# variable whose default reproduces the old GLIBC_2.(1[89]|[2-9][0-9]) match.
#
# Gates each binary on three things:
#   1. No GLIBC symbols above the profile ceiling.
#   2. Only the documented dynamic deps (whitelist below).
#   3. No forbidden API symbols (kernel/glibc ceiling for the profile).
#
# Fail-fast for `make release` (CI calls release, which depends on verify).
# ----------------------------------------------------------------------
ALLOWED_DLLS := linux-vdso libc libpthread libdl libm libresolv librt ld-linux-x86-64

# --- Profile knobs. Override via `make VERIFY_BIN=... GLIBC_FORBID_RE=... verify`
#     but normally consumed through verify / verify-legacy below.
VERIFY_BIN        ?= $(BIN)
VERIFY_PROFILE    ?= manylinux2014
# Modern ceiling: glibc 2.17 → forbid 2.18 and any 2.[2-9][0-9].
GLIBC_FORBID_RE   ?= GLIBC_2\.(1[89]|[2-9][0-9])
GLIBC_CEILING_MSG ?= GLIBC 2.18+
# Forbidden APIs introduced after the ceiling. Modern set (post glibc 2.17 /
# kernel 3.10). Legacy adds more (see verify-legacy) since glibc 2.12 lacks
# several of these wrappers too.
FORBID_API_RE     ?=  U (getrandom|statx|memfd_create|renameat2|copy_file_range|pidfd_)

# verify_body — shared recipe (used by both verify and verify-legacy).
define verify_body
	@echo "[verify] $(VERIFY_BIN) — $(VERIFY_PROFILE) ABI compliance"
	@bad=$$(objdump -T $(VERIFY_BIN) 2>/dev/null | awk '/GLIBC_/ {print $$5}' \
	         | grep -E '$(GLIBC_FORBID_RE)' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: $(GLIBC_CEILING_MSG) symbols found (breaks $(VERIFY_PROFILE) ABI):"; echo "$$bad"; exit 1; fi
	@deps=$$(ldd $(VERIFY_BIN) | awk '/=>/ {print $$1} /linux-vdso/ {print $$1}' \
	         | sed -E 's/\.so.*$$//' | sort -u); \
	 for d in $$deps; do \
	   case " $(ALLOWED_DLLS) " in *" $$d "*) ;; \
	     *) echo "[verify] FAIL: unexpected dynamic dep '$$d.so'"; \
	        echo "[verify]   allowed: $(ALLOWED_DLLS)"; exit 1 ;; \
	   esac; \
	 done
	@bad=$$(nm -D $(VERIFY_BIN) 2>/dev/null | grep -E '$(FORBID_API_RE)' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: forbidden APIs ($(VERIFY_PROFILE) ceiling):"; echo "$$bad"; exit 1; fi
	@echo "[verify] OK: $(VERIFY_PROFILE) clean, dyn-dep whitelist matches, no forbidden APIs"
endef

verify: $(BIN)
	$(verify_body)

# verify-legacy — manylinux2010 / glibc 2.12 / CentOS 6 ceiling.
# Forbids GLIBC_2.13+ (i.e. 2.13..2.19 and any 2.[2-9][0-9]). Also forbids a
# wider API set: secure_getenv (2.17), __ppoll_chk etc. are not present pre
# 2.12, plus the modern set. Runs against the legacy binary.
verify-legacy: $(LEGACY_BIN)
	$(MAKE) verify \
	  VERIFY_BIN=$(LEGACY_BIN) \
	  VERIFY_PROFILE=manylinux2010 \
	  GLIBC_CEILING_MSG="GLIBC 2.13+" \
	  GLIBC_FORBID_RE='GLIBC_2\.(1[3-9]|[2-9][0-9])' \
	  FORBID_API_RE=' U (getrandom|statx|memfd_create|renameat2|copy_file_range|pidfd_|secure_getenv|getauxval)'

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

# ----------------------------------------------------------------------
# release-legacy — manylinux2010 / glibc 2.12 / CentOS 6 binary.
#
# Produces a SEPARATE artifact (assessment-agent-legacy + the abi-tagged
# dist name) so it never collides with the modern dist/. The build inputs
# (sources, CFLAGS, vendored static-link set) are identical — only the
# build HOST differs (older glibc) and the verify ceiling is glibc 2.12.
# The modern `release` path above is untouched.
#
# Usage (from a manylinux2010 build host — glibc 2.12):
#   docker run --rm -v $(pwd):/src -w /src quay.io/pypa/manylinux2010_x86_64 \
#       bash -lc 'make vendor-fetch && make vendor-build && \
#                 make USE_VENDORED=1 release-legacy'
#
# manylinux2010 reached EOL upstream; if the image is unavailable, any
# CentOS 6 / glibc-2.12 toolchain box works — the verify-legacy gate is the
# real ABI guarantee, not the specific image. USE_VENDORED=1 statically links
# cJSON/rabbitmq-c/curl/libarchive/OpenSSL/zlib exactly as the modern build,
# so only the glibc-family libs stay dynamic against the 2.12 baseline.
#
# The legacy binary is the one install.sh ships to CentOS/RHEL/Oracle Linux 6
# (SysV path). See docs/centos6-bringup.md.
# ----------------------------------------------------------------------
LEGACY_BIN      := assessment-agent-legacy
LEGACY_DIST_BIN := $(DIST_DIR)/assessment-agent-linux-x86_64-glibc2.12

# Build the legacy binary from the same objects + embed blob as $(BIN). We
# depend on $(BIN) so the standard compile runs, then copy it under the legacy
# name. (The ABI floor is a property of the build HOST's glibc, not of extra
# compiler flags — so the bytes are identical to $(BIN) on a 2.12 host; the
# separate name exists so verify-legacy and the dist artifact are unambiguous.)
$(LEGACY_BIN): $(BIN)
	cp $(BIN) $(LEGACY_BIN)

release-legacy: $(LEGACY_BIN) verify-legacy
	@mkdir -p $(DIST_DIR)
	cp $(LEGACY_BIN) $(LEGACY_DIST_BIN)
	cd $(DIST_DIR) && sha256sum $(notdir $(LEGACY_DIST_BIN)) >> SHA256SUMS
	@echo "[release-legacy] packaged $(LEGACY_DIST_BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean test-unit \
        vendor-fetch vendor-build vendor-clean \
        vendor-build-openssl vendor-build-zlib vendor-build-cjson \
        vendor-build-rabbitmq vendor-build-curl vendor-build-libarchive \
        verify verify-legacy release release-legacy
