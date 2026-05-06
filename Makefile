CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?=

# AGENT_VERSION can be overridden at build time:
#   make AGENT_VERSION=1.2.3
AGENT_VERSION ?= 1.0.0

# ----------------------------------------------------------------------
# Library resolution
#   default      : pkg-config (system-installed librabbitmq + libcjson)
#   USE_VENDORED : link statically against vendor/{cJSON,rabbitmq-c}
# ----------------------------------------------------------------------
PKGS               := librabbitmq libcjson

VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION := v0.15.0
CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a

ifeq ($(USE_VENDORED),1)
  CFLAGS += -I$(CJSON_DIR) \
            -I$(RABBITMQ_C_DIR)/librabbitmq \
            -I$(RABBITMQ_C_DIR)/build/librabbitmq
  LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) -lssl -lcrypto -lpthread
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
               $(shell pkg-config --cflags libcjson)
TEST_LDLIBS := $(shell pkg-config --libs libcjson)

$(TEST_BIN): tests/unit/test_main.c tests/unit/tinytest.h src/util.c src/collect.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/unit/test_main.c src/util.c $(TEST_LDLIBS)

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

# ----------------------------------------------------------------------
# Vendoring — fetches cJSON and rabbitmq-c into vendor/, builds static libs.
#   make vendor-fetch     # one-time clone (git, network required)
#   make vendor-build     # configure + compile static libs (cmake required)
#   make USE_VENDORED=1   # build the agent against vendored static libs
# ----------------------------------------------------------------------
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR) || git clone --depth 1 --branch $(CJSON_VERSION) https://github.com/DaveGamble/cJSON.git $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git $(RABBITMQ_C_DIR)

vendor-build: vendor-fetch
	cmake -S $(CJSON_DIR) -B $(CJSON_DIR)/build -DCJSON_BUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(CJSON_DIR)/build --target cjson
	cmake -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DENABLE_SSL_SUPPORT=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	cmake --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

.PHONY: all clean test-unit vendor-fetch vendor-build vendor-clean
