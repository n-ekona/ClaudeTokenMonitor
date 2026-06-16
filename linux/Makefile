CC      ?= cc
PKGS    := gtk+-3.0 webkit2gtk-4.1 json-glib-1.0
# webkit2gtk-4.1 が無ければ 4.0 を使う
ifeq ($(shell pkg-config --exists webkit2gtk-4.1 && echo yes),)
PKGS    := gtk+-3.0 webkit2gtk-4.0 json-glib-1.0
endif

CFLAGS  += -O2 -Wall $(shell pkg-config --cflags $(PKGS))
LIBS    := $(shell pkg-config --libs $(PKGS))
SRCS    := src/main.c src/stats.c
BIN     := tokenmonitor

all: $(BIN)

$(BIN): $(SRCS) src/stats.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
