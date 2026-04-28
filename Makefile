CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?=

# AGENT_VERSION can be overridden at build time:
#   make AGENT_VERSION=1.2.3
AGENT_VERSION ?= 1.0.0

PKGS    := librabbitmq libcjson
CFLAGS  += -Iinclude $(shell pkg-config --cflags $(PKGS)) \
           -DAGENT_VERSION=\"$(AGENT_VERSION)\"
LDLIBS  := $(shell pkg-config --libs $(PKGS))

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

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

.PHONY: all clean test-unit
