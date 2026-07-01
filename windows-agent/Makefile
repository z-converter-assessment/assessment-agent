# ============================================================
# assessment-agent (Windows) — MinGW-w64 build
#
# Build host expectation: MSYS2 + mingw-w64 toolchain on Windows.
# `gcc`/`ar`/`cmake` in PATH resolve to the mingw64 toolchain.
#
# Usage:
#   make vendor-fetch   # one-time: git clone vendor deps
#   make vendor-build   # configure + compile static libs (cmake required)
#   make                # build assessment-agent-win2016-x64.exe (default PROFILE=modern, static)
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
WINDRES ?= windres
JOBS    ?= 4

# ----------------------------------------------------------------------
# Build profile — NT generations from one source tree
# ----------------------------------------------------------------------
#   PROFILE=modern (default) : NT 10.0 — Server 2016/2019/2022/2025, Win10/11.
#                              OpenSSL 3.x, _WIN32_WINNT=0x0A00.
#   PROFILE=win7             : NT 6.1–6.3 — Server 2008R2 / 2012 / 2012R2,
#                              Win7/8/8.1. OpenSSL 3.x (still supports Win7),
#                              only the API ceiling drops to 0x0601.
#   PROFILE=legacy32         : NT 5.2, **i686 (32-bit)** — Server 2003 x86 / XP.
#                              OpenSSL 1.0.2u (last XP-capable LTS), 0x0502,
#                              bcrypt dropped from the import table. This is the
#                              SOLE 2003 target: the mass-market 2003 SKU is x86
#                              and the x64 Edition was rare, so no x64 build is
#                              produced. Needs the win32/threads NT5.2 patch
#                              (SRWLock -> CRITICAL_SECTION); see
#                              agent-fleet/win-legacy-patch.
#
#   make PROFILE=win7     vendor-build && make PROFILE=win7     release
#   make PROFILE=legacy32 vendor-build && make PROFILE=legacy32 release
#
# The C code is identical across profiles: GetTickCount64 / BCryptGenRandom
# are resolved at runtime (monotonic_ms / compat_rand_bytes), so a lower
# WINNT_VER never leaves an unresolved import. Knobs that differ:
#   - WINNT_VER          : API surface to compile against.
#   - OPENSSL_VERSION    : 3.x dropped XP/2003; legacy32 pins 1.0.2u. Override
#     with OPENSSL_VERSION= if a 1.1.1 build proves to handshake (TLS PoC).
#   - CURL_VERSION       : legacy32 pins a curl that still builds on 1.0.2.
#   - BCRYPT_LIB         : empty on legacy32 — no static -lbcrypt (Server 2003
#     has no bcrypt.dll; a static import would break load).
#   - BIN suffix         : all binaries coexist in dist/.
# NOTE: legacy32 needs an OLDER MinGW-w64 (i686) that still targets NT 5.2
# (recent MinGW dropped XP/2003 startup support). modern and win7 build fine
# on the current MSYS2/mingw64 toolchain.
PROFILE ?= modern

ifeq ($(PROFILE),legacy32)
  # legacy32 = legacy(NT 5.2) but **32-bit (i686)**. The mass-market Server 2003
  # SKU is x86; the x64 Edition was rare. The legacy(x64) binary above never
  # loads on those hosts, so without this profile "we target 2003" is unmet.
  # Identical NT 5.2 ceiling, OpenSSL 1.0.2u and curl/rabbitmq pins as legacy —
  # ONLY the architecture changes: i686 toolchain + the OpenSSL `mingw` (32-bit)
  # target. Cross-built from an x64 host; override CC/AR/WINDRES/OPENSSL_CROSS
  # if you build inside a native mingw32 shell. Like legacy, it needs an OLDER
  # MinGW-w64 (i686) that still emits NT 5.2 startup code.
  CC                  := i686-w64-mingw32-gcc
  AR                  := i686-w64-mingw32-ar
  WINDRES             := i686-w64-mingw32-windres
  OPENSSL_TARGET      := mingw
  OPENSSL_CROSS       := --cross-compile-prefix=i686-w64-mingw32-
  WINNT_VER           := 0x0502
  OPENSSL_VERSION     ?= OpenSSL_1_0_2u
  CURL_VERSION        ?= curl-8_10_1
  RABBITMQ_C_VERSION  ?= v0.10.0
  BIN                 := assessment-agent-win2003-x86.exe
  BCRYPT_LIB          :=
  OPENSSL_CONFIG_OPTS := no-shared no-dso \
      -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
      -Wno-error=int-conversion -Wno-error=implicit-int -fcommon
  EXTRA_SYS_LIBS      := -lgdi32 -lssp
  VENDOR_CFLAGS       := -D_WIN32_WINNT=0x0502 -DWINVER=0x0502
  ALLOWED_DLLS        := kernel32|advapi32|user32|gdi32|ws2_32|iphlpapi|crypt32|secur32|ole32|oleaut32|shell32|msvcrt|ntdll
else ifeq ($(PROFILE),win7)
  WINNT_VER           := 0x0601
  OPENSSL_VERSION     ?= openssl-3.0.15
  CURL_VERSION        ?= curl-8_10_1
  BIN                 := assessment-agent-win2008r2-x64.exe
  BCRYPT_LIB          := -lbcrypt
  OPENSSL_CONFIG_OPTS := no-shared no-dso no-engine no-tests
  ALLOWED_DLLS        := kernel32|advapi32|user32|ws2_32|iphlpapi|crypt32|secur32|bcrypt|ole32|oleaut32|shell32|msvcrt|ntdll|api-ms-win-.+
else
  WINNT_VER           := 0x0A00
  OPENSSL_VERSION     ?= openssl-3.0.15
  CURL_VERSION        ?= curl-8_10_1
  BIN                 := assessment-agent-win2016-x64.exe
  BCRYPT_LIB          := -lbcrypt
  OPENSSL_CONFIG_OPTS := no-shared no-dso no-engine no-tests
  ALLOWED_DLLS        := kernel32|advapi32|user32|ws2_32|iphlpapi|crypt32|secur32|bcrypt|ole32|oleaut32|shell32|msvcrt|ntdll|api-ms-win-.+
endif

# Profile-specific extras (empty on modern/win7; set in the legacy block).
EXTRA_SYS_LIBS ?=
VENDOR_CFLAGS  ?=

CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11 \
           -D_WIN32_WINNT=$(WINNT_VER) -DWINVER=$(WINNT_VER) -DUNICODE -D_UNICODE \
           -DCURL_STATICLIB -DAMQP_STATIC
LDFLAGS ?= -static -static-libgcc -Wl,--no-insert-timestamp

AGENT_VERSION ?= 1.0.0

# ----------------------------------------------------------------------
# Vendor static libraries (gitignored — built locally before agent build)
# ----------------------------------------------------------------------
VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION ?= v0.15.0
ZLIB_VERSION       := v1.3.1
# CURL_VERSION / OPENSSL_VERSION are set by the build-profile block above
# (modern vs LEGACY) so they can be pinned per ABI target.

CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CURL_DIR           := $(VENDOR_DIR)/curl
OPENSSL_DIR        := $(VENDOR_DIR)/openssl
ZLIB_DIR           := $(VENDOR_DIR)/zlib

CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
CURL_LIB           := $(CURL_DIR)/build/lib/libcurl.a
# curl >= 7.77 splits the static lib into the `libcurl_static` target; older
# curl (legacy pins 7.64.1) only exposes `libcurl`. Profile blocks override.
CURL_STATIC_TARGET ?= libcurl_static
OPENSSL_SSL        := $(OPENSSL_DIR)/install/lib/libssl.a
OPENSSL_CRYPTO     := $(OPENSSL_DIR)/install/lib/libcrypto.a
ZLIB_LIB           := $(ZLIB_DIR)/libz.a

OPENSSL_TARGET     ?= mingw64
OPENSSL_CROSS      ?=

CFLAGS += -I$(CJSON_DIR) \
          -I$(RABBITMQ_C_DIR)/include \
          -I$(RABBITMQ_C_DIR)/build/include \
          -I$(RABBITMQ_C_DIR)/librabbitmq \
          -I$(RABBITMQ_C_DIR)/build/librabbitmq \
          -I$(CURL_DIR)/include \
          -I$(OPENSSL_DIR)/install/include \
          -I$(ZLIB_DIR) \
          -Iinclude \
          -DAGENT_VERSION=\"$(AGENT_VERSION)\"

# Static vendor archives + Windows system DLLs (kept dynamic — they ship
# with every supported Windows version, see deploy/SUPPORTED_OS.md):
#   ws2_32   : Winsock (AMQP socket, gethostname, inet_ntop)
#   iphlpapi : GetAdaptersAddresses, GetIfTable2 (network inventory + io)
#   advapi32 : Registry, Service Control Manager, ACL (SetNamedSecurityInfoW)
#   crypt32  : OpenSSL on Windows needs CertOpenSystemStoreW etc.
#   secur32  : SSPI (used transitively by some Schannel paths)
#   bcrypt   : BCryptGenRandom (modern profile only — resolved at runtime via
#              compat_rand_bytes(), so LEGACY drops the static import)
#   ole32 / oleaut32 : COM init (used by some Windows API surfaces)
#   shell32  : installer subcommand — SHCreateDirectoryExW, ShellExecuteW
LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
          -lws2_32 -liphlpapi -ladvapi32 -lkernel32 -luser32 \
          -lcrypt32 -lsecur32 $(BCRYPT_LIB) -lole32 -loleaut32 -lshell32 $(EXTRA_SYS_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
RES := src/resources.res
# BIN is set by the build-profile block above (per-profile names like
# assessment-agent-win2016-x64.exe / -win2003-x86.exe) so all profiles coexist in dist/.

all: $(BIN)

$(BIN): $(OBJ) $(RES)
	$(CC) $(OBJ) $(RES) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# PE resource — embeds deploy/agent.env.example as RT_RCDATA so the installer
# subcommand (e.g. assessment-agent-win2016-x64.exe install) can seed and prompt without any
# external files on the target host.
# `-I .` so the `deploy/agent.env.example` reference in resources.rc resolves
# against the windows-agent/ root (windres' default is the .rc file's dir).
$(RES): src/resources.rc deploy/agent.env.example
	$(WINDRES) -O coff -I . -i $< -o $@

# ----------------------------------------------------------------------
# Vendor fetch (one-time clone — network required)
# ----------------------------------------------------------------------
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git  $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git $(RABBITMQ_C_DIR)
	@test -d $(CURL_DIR)       || git clone --depth 1 --branch $(CURL_VERSION)       https://github.com/curl/curl.git         $(CURL_DIR)
	@test -d $(OPENSSL_DIR)    || git clone --depth 1 --branch $(OPENSSL_VERSION)    https://github.com/openssl/openssl.git   $(OPENSSL_DIR)
	@test -d $(ZLIB_DIR)       || git clone --depth 1 --branch $(ZLIB_VERSION)       https://github.com/madler/zlib.git       $(ZLIB_DIR)

# ----------------------------------------------------------------------
# Vendor build — MinGW64 static
# ----------------------------------------------------------------------
vendor-build: vendor-fetch vendor-build-openssl vendor-build-zlib vendor-build-cjson vendor-build-rabbitmq vendor-build-curl

vendor-build-openssl:
	# `--libdir=lib` 명시 — OpenSSL 3.x 가 host triplet 에 따라 `lib64` 또는
	# `lib/<host>` 로 install 하면 Makefile 의 $(OPENSSL_SSL) 고정 경로
	# (`install/lib/libssl.a`) 와 어긋난다.
	cd $(OPENSSL_DIR) && ./Configure $(OPENSSL_TARGET) $(OPENSSL_CONFIG_OPTS) $(OPENSSL_CROSS) \
	    --libdir=lib --prefix=$(CURDIR)/$(OPENSSL_DIR)/install
	# 1.0.2 install_sw's mkdir is NOT idempotent — it dies with
	# "Cannot create directory .../install: File exists" (Error 17) when the
	# prefix dir already exists. Wipe it first so install starts clean.
	rm -rf $(OPENSSL_DIR)/install
	$(MAKE) -C $(OPENSSL_DIR) -j$(JOBS)
	$(MAKE) -C $(OPENSSL_DIR) install_sw

vendor-build-zlib:
	# zlib win32/Makefile.gcc 가 ARFLAGS=rcs 를 자체 정의함. AR 변수에는
	# 명령만 전달 (옵션 X) — 그렇지 않으면 `ar rc rcs libz.a ...` 으로
	# rcs 가 파일명으로 해석되어 실패.
	$(MAKE) -C $(ZLIB_DIR) -f win32/Makefile.gcc CC=$(CC) AR=$(AR) libz.a

vendor-build-cjson:
	$(CMAKE) -S $(CJSON_DIR) -B $(CJSON_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(CJSON_DIR)/build --target cjson -- -j$(JOBS)

vendor-build-rabbitmq:
	# GCC 14+ (MSYS2 mingw64 최신) 가 -Wincompatible-pointer-types 를 error 로
	# promote — rabbitmq-c v0.15.0 amqp_socket.c:309 의 ioctlsocket(int*) ↔
	# Windows winsock2.h 의 u_long* 미스매치를 거부. upstream fix 전까지 demote.
	$(CMAKE) -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types $(VENDOR_CFLAGS)" \
	      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	      -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
	      -DENABLE_SSL_SUPPORT=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DCMAKE_PREFIX_PATH=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_INCLUDE_DIR=$(CURDIR)/$(OPENSSL_DIR)/install/include \
	      -DOPENSSL_SSL_LIBRARY=$(CURDIR)/$(OPENSSL_DIR)/install/lib/libssl.a \
	      -DOPENSSL_CRYPTO_LIBRARY=$(CURDIR)/$(OPENSSL_DIR)/install/lib/libcrypto.a \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static -- -j$(JOBS)
	# Windows mingw cmake 가 정적 라이브러리에 `lib` prefix 를 자동 추가하면서
	# SOVERSION 까지 붙여 `liblibrabbitmq.<N>.a` 형태로 출력한다. Linux 와 동일
	# 경로 (`librabbitmq.a`) 로 별칭을 만들어 $(RABBITMQ_C_LIB) 경로를 통일.
	cp $(RABBITMQ_C_DIR)/build/librabbitmq/liblibrabbitmq.*.a \
	   $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a

vendor-build-curl:
	# libcurl static, OpenSSL backend, IDN/LDAP/FTP 등 비사용 옵션 비활성.
	# MSYS2 mingw64 환경엔 libpsl / libssh2 / libidn2 / brotli / zstd / nghttp2 등이
	# 설치돼 있어 cmake auto-detection 이 자동 ON 처리 → libcurl.a 가 그 의존성에
	# 미해결 심볼을 남긴다. Linux manylinux2014 와 동일한 최소 의존 빌드를 강제하기
	# 위해 모든 선택적 의존을 명시적으로 OFF.
	$(CMAKE) -S $(CURL_DIR) -B $(CURL_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DCMAKE_C_FLAGS="$(VENDOR_CFLAGS)" \
	      -DBUILD_SHARED_LIBS=OFF -DCURL_STATICLIB=ON \
	      -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
	      -DCURL_USE_OPENSSL=ON -DCMAKE_USE_OPENSSL=ON \
	      -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_FTP=ON \
	      -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
	      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_POP3=ON \
	      -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON \
	      -DCURL_USE_LIBPSL=OFF  -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
	      -DCURL_USE_LIBIDN2=OFF -DCURL_USE_GSASL=OFF   -DCURL_USE_GSSAPI=OFF \
	      -DCURL_BROTLI=OFF       -DCURL_ZSTD=OFF        -DUSE_NGHTTP2=OFF \
	      -DUSE_NGTCP2=OFF        -DUSE_QUICHE=OFF       -DUSE_LIBRTMP=OFF \
	      -DCURL_DISABLE_IDN=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibPSL=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Libssh2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibSSH=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Libidn2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibIDN2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Brotli=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Zstd=ON    \
	      -DCMAKE_DISABLE_FIND_PACKAGE_NGHTTP2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_nghttp2=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(CURL_DIR)/build --target $(CURL_STATIC_TARGET) -- -j$(JOBS)
	# Windows mingw cmake 출력 이름은 `libcurl.a` 가 아닐 수 있어 — `libcurl_static`
	# target 의 실제 파일을 $(CURL_LIB) 경로(`build/lib/libcurl.a`) 로 별칭.
	@if [ ! -f $(CURL_DIR)/build/lib/libcurl.a ]; then \
		cp $$(ls $(CURL_DIR)/build/lib/lib*curl*.a | head -n1) \
		   $(CURL_DIR)/build/lib/libcurl.a ; \
	fi

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(RES) $(BIN) build-deps.txt

# ----------------------------------------------------------------------
# Verify — only Windows system DLLs allowed in import table
# ----------------------------------------------------------------------
verify: $(BIN)
	@echo "[verify] checking DLL dependencies of $(BIN)..."
	@objdump -p $(BIN) | awk '/DLL Name:/ {print tolower($$3)}' | sort -u > build-deps.txt
	@cat build-deps.txt
	@if grep -vE '^($(ALLOWED_DLLS))\.dll$$' build-deps.txt; then \
		echo "[verify] FAIL: non-system DLL dependency listed above"; \
		echo "[verify]   (profile: LEGACY=$(LEGACY), allowed: $(ALLOWED_DLLS))"; \
		exit 1; \
	fi
	@echo "[verify] OK: only Windows system DLLs referenced (LEGACY=$(LEGACY))"

# ----------------------------------------------------------------------
# Release packaging — dist/$(BIN) + SHA256SUMS
# ----------------------------------------------------------------------
DIST_DIR := dist

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_DIR)/
	cd $(DIST_DIR) && sha256sum $(BIN) > SHA256SUMS
	@echo "[release] packaged $(DIST_DIR)/$(BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean vendor-fetch vendor-build vendor-build-openssl vendor-build-zlib \
        vendor-build-cjson vendor-build-rabbitmq vendor-build-curl vendor-clean verify release
