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

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
