CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS   ?= -pthread

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:.c=.o)
BIN      := pi-mc-bridge

# Tests: link every src object except main.o against the test sources.
LIBOBJ   := $(filter-out src/main.o,$(OBJ))
TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := tests/run-tests

.PHONY: all clean test check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(TEST_SRC) $(LIBOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ $(TEST_SRC) $(LIBOBJ) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

check: test

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)
