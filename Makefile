CC = gcc

# https://wiki.debian.org/Hardening
# $ hardening-check out
DPKG_EXPORT_BUILDFLAGS = 1

PKGS = x11 cairo pangocairo
C := $(shell pkg-config --cflags $(PKGS))
L := $(shell pkg-config --libs $(PKGS))

CFLAGS := $(shell dpkg-buildflags --get CFLAGS) 
LDFLAGS := $(shell dpkg-buildflags --get LDFLAGS) 

CFLAGS += -D_FORTIFY_SOURCE=3 -fstack-protector-all
CFLAGS += $(C) $(L)
CFLAGS += -DDEBUG=0

BIN := out

debug: CFLAGS := -ggdb3 \
	-pedantic -W -Wall -Wstrict-prototypes -Wunreachable-code  \
	-Wwrite-strings -Wpointer-arith -Wbad-function-cast \
	-Wcast-align -Wcast-qual \
	-Wfree-nonheap-object
debug: CFLAGS += $(C) $(L)
debug: CFLAGS += -DDEBUG=1

fanalyzer: CFLAGS += -g -O1 -fanalyzer

sanitize: CFLAGS += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

SRC := src/main.c src/editor.c src/buffer.c src/render.c

all: main
debug: main
sanitize: main
fanalyzer: main

main: $(SRC)
	$(CC) -o $(BIN) $(SRC) $(CFLAGS) $(LDFLAGS) 2>&1 | tee -a out.log;

clean:
	rm -rfv $(BIN) reports src/*.o *.s *.bc *.db *.log

install:
	cp $(BIN) ${HOME}/.local/bin/xenoed
	chmod 755 ${HOME}/.local/bin/xenoed

uninstall:
	rm ${HOME}/.local/bin/xenoed


