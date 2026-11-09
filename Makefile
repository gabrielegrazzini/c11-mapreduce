CC := gcc
CFLAGS := -std=gnu11 -Wall -Iinclude -Isrc -pthread
AR := ar
ARFLAGS := rcs

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

LIB := libmr.a

WC_DIR := examples/word_count
WC_SRC := $(WC_DIR)/wc.c
WC_BIN := $(WC_DIR)/wc
WC_OUTPUT := $(WC_DIR)/output.mro

WL_DIR := examples/word_length
WL_SRC := $(WL_DIR)/wl.c
WL_BIN := $(WL_DIR)/wl
WL_OUTPUT := $(WL_DIR)/output.mro

.PHONY: default test wc wl clean

default: $(LIB) $(WC_BIN) $(WL_BIN)

$(LIB): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(WC_BIN): $(WC_SRC) $(LIB)
	$(CC) $(CFLAGS) $(WC_SRC) -L. -lmr -o $@

$(WL_BIN): $(WL_SRC) $(LIB)
	$(CC) $(CFLAGS) $(WL_SRC) -L. -lmr -o $@

test: wc wl

wc: $(WC_BIN)
	./$(WC_BIN)

wl: $(WL_BIN)
	./$(WL_BIN)


clean:
	rm -f $(OBJ) $(LIB)
	rm -f $(WC_BIN) $(WC_OUTPUT)
	rm -f $(WL_BIN) $(WL_OUTPUT)
	rm -f mr.log