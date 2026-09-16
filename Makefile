CC ?= gcc

# https://wiki.debian.org/Hardening
# $ hardening-check out
DPKG_EXPORT_BUILDFLAGS = 1

PKGS = x11 cairo pangocairo
C := $(shell pkg-config --cflags $(PKGS))
L := $(shell pkg-config --libs $(PKGS))

CFLAGS ?= $(shell dpkg-buildflags --get CFLAGS)
LDFLAGS ?= $(shell dpkg-buildflags --get LDFLAGS)

CFLAGS += -D_FORTIFY_SOURCE=3 -fstack-protector-all
CFLAGS += $(C) $(L)
CFLAGS += -DDEBUG=0

BIN := xenoed
TEST_BIN := tests/test_repeat
TEST_SRC := tests/test_repeat.c src/editor.c src/buffer.c src/repeat.c

# Keep the normal build warning level modest; debug builds enable the
# stricter diagnostics used for development.
debug: CFLAGS := -ggdb3 \
	-pedantic -W -Wall -Wstrict-prototypes -Wunreachable-code \
	-Wwrite-strings -Wpointer-arith -Wbad-function-cast \
	-Wcast-align -Wcast-qual \
	-Wfree-nonheap-object
debug: CFLAGS += $(C) $(L)
debug: CFLAGS += -DDEBUG=1

fanalyzer: CFLAGS += -g -O1 -fanalyzer

sanitize: CFLAGS += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

SRC := src/main.c src/editor.c src/buffer.c src/render.c src/repeat.c src/cmdhist.c

all: main
debug: main
sanitize: main
fanalyzer: main

test: $(TEST_BIN)
	./$(TEST_BIN)

main: $(SRC)
	$(CC) -o $(BIN) $(SRC) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=editor_init -Wl,--wrap=editor_handle_key 2>&1 | tee out.log;

$(TEST_BIN): $(TEST_SRC)
	$(CC) -std=c11 -O1 -Wall -Wextra -Isrc -o $@ $(TEST_SRC) -Wl,--wrap=editor_init -Wl,--wrap=editor_handle_key

test-cmdhist: tests/test_cmdhist.c src/cmdhist.c
	$(CC) -std=c11 -O1 -Wall -Wextra -Isrc -o tests/test_cmdhist tests/test_cmdhist.c src/cmdhist.c
	./tests/test_cmdhist

lint:
	cppcheck --enable=warning,style,performance,portability --error-exitcode=1 --inline-suppr $(SRC) 2>&1 | tee lint.log;

clean:
	rm -rfv $(BIN) $(TEST_BIN) tests/test_cmdhist reports src/*.o *.s *.bc *.db *.log

install:
	cp $(BIN) ${HOME}/.local/bin/xenoed
	chmod 755 ${HOME}/.local/bin/xenoed

uninstall:
	rm ${BIN} ${HOME}/.local/bin/xenoed


