CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?=

# AGENT_VERSION can be overridden at build time:
#   make AGENT_VERSION=1.2.3
AGENT_VERSION ?= 1.0.0

# ----------------------------------------------------------------------
# Library resolution
#   default      : pkg-config (system-installed libs)
#   USE_VENDORED : link statically against vendor/{cJSON,rabbitmq-c,curl,libarchive}
#
# Worker (task.install) adds libcurl (HTTPS download + sha256 via
# OpenSSL EVP) and libarchive (tar extract with entry-type/permission
# filtering). OpenSSL is shared with rabbitmq-c.
# ----------------------------------------------------------------------
PKGS               := librabbitmq libcjson libcurl libarchive

VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION := v0.15.0
CURL_VERSION       := curl-8_10_1
LIBARCHIVE_VERSION := v3.7.7
CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CURL_DIR           := $(VENDOR_DIR)/curl
LIBARCHIVE_DIR     := $(VENDOR_DIR)/libarchive
CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
CURL_LIB           := $(CURL_DIR)/build/lib/libcurl.a
LIBARCHIVE_LIB     := $(LIBARCHIVE_DIR)/build/libarchive/libarchive.a

ifeq ($(USE_VENDORED),1)
  CFLAGS += -I$(CJSON_DIR) \
            -I$(RABBITMQ_C_DIR)/librabbitmq \
            -I$(RABBITMQ_C_DIR)/build/librabbitmq \
            -I$(CURL_DIR)/include \
            -I$(LIBARCHIVE_DIR)/libarchive
  LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(LIBARCHIVE_LIB) \
            -lssl -lcrypto -lz -lpthread
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
# ----------------------------------------------------------------------
TEST_BIN    := tests/unit/run_unit
TEST_CFLAGS := -Wall -Wextra -Wpedantic -Wno-unused-function -O0 -g -std=c11 \
               -Iinclude -DAGENT_VERSION=\"test\" \
               $(shell pkg-config --cflags libcjson libcurl libarchive)
# download.c / extract.c are compile-included by tests; their transitive
# symbol references (libcurl, libarchive, OpenSSL EVP) must be linked.
TEST_LDLIBS := $(shell pkg-config --libs libcjson libcurl libarchive) -lcrypto

$(TEST_BIN): tests/unit/test_main.c tests/unit/tinytest.h \
             src/util.c src/collect.c src/download.c src/extract.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/unit/test_main.c src/util.c $(TEST_LDLIBS)

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

# ----------------------------------------------------------------------
# Vendoring — fetches deps into vendor/, builds static libs.
#   make vendor-fetch     # one-time clone (git, network required)
#   make vendor-build     # configure + compile static libs (cmake required)
#   make USE_VENDORED=1   # build the agent against vendored static libs
# ----------------------------------------------------------------------
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git       $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git      $(RABBITMQ_C_DIR)
	@test -d $(CURL_DIR)       || git clone --depth 1 --branch $(CURL_VERSION)       https://github.com/curl/curl.git              $(CURL_DIR)
	@test -d $(LIBARCHIVE_DIR) || git clone --depth 1 --branch $(LIBARCHIVE_VERSION) https://github.com/libarchive/libarchive.git  $(LIBARCHIVE_DIR)

vendor-build: vendor-fetch
	cmake -S $(CJSON_DIR) -B $(CJSON_DIR)/build -DCJSON_BUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(CJSON_DIR)/build --target cjson
	cmake -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DENABLE_SSL_SUPPORT=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static
	cmake -S $(CURL_DIR) -B $(CURL_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DCURL_STATICLIB=ON \
	      -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
	      -DCURL_USE_OPENSSL=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_FTP=ON \
	      -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
	      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_POP3=ON \
	      -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(CURL_DIR)/build --target libcurl_static
	cmake -S $(LIBARCHIVE_DIR) -B $(LIBARCHIVE_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_TEST=OFF \
	      -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
	      -DENABLE_OPENSSL=ON \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(LIBARCHIVE_DIR)/build --target archive_static

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

.PHONY: all clean test-unit vendor-fetch vendor-build vendor-clean
