CC ?= cc
PKGS = x11 cairo pangocairo
BASE_CFLAGS = -O2 -g -Wall -Wextra -std=c11 $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

# For build-time overrides that don't touch source, e.g.:
#   make EXTRA_CFLAGS='-DXENOED_FONT="Georgia 14"'
# A separate variable (rather than just appending to CFLAGS in this
# Makefile) because `make CFLAGS=...` on the command line takes precedence
# over -- and completely replaces, not merges with -- any CFLAGS this
# Makefile itself sets, which would silently drop BASE_CFLAGS above
# (including the pkg-config include paths) rather than add to it.
EXTRA_CFLAGS ?=
CFLAGS = $(BASE_CFLAGS) $(EXTRA_CFLAGS)

SRC = src/main.c src/editor.c src/buffer.c src/render.c
OBJ = $(SRC:.c=.o)
BIN = xenoed

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)
