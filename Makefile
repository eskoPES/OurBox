# JackBox Windows components (cross-built with MinGW-w64; also builds natively
# on Windows with MSYS2 MinGW-w64).
CC      ?= x86_64-w64-mingw32-gcc
TCC     := clang --target=x86_64-w64-mingw32 -fuse-ld=lld
TCXX    := clang++ --target=x86_64-w64-mingw32 -fuse-ld=lld
CXX     ?= x86_64-w64-mingw32-g++
DLLTOOL ?= x86_64-w64-mingw32-dlltool
OUT     := build/win64
CFLAGS  := -O2 -g -Wall -Wextra -Wno-cast-function-type -Wno-array-bounds -Wno-missing-field-initializers
LDSTATIC:= -static-libgcc

ifeq ($(origin CC),default)
CC := x86_64-w64-mingw32-gcc
endif

RUNTIME_SRC := $(wildcard runtime/xgameruntime/*.c)
TESTS_C     := $(patsubst tests/titles/%.c,$(OUT)/titles/%.exe,$(wildcard tests/titles/*.c))
TESTS_CXX   := $(patsubst tests/titles/%.cpp,$(OUT)/titles/%.exe,$(wildcard tests/titles/*.cpp))

TLS_HELPERS := $(foreach n,1 2 3 4 5 6 7,$(OUT)/jb_tls$(n).dll)

all: $(OUT)/jackbox.exe $(OUT)/xgameruntime.dll $(TLS_HELPERS) $(OUT)/titles/MicrosoftGame.config titles

$(OUT)/jb_tls%.dll: runtime/tlsslot/jb_tls.c | $(OUT)
	$(TCC) -O2 -shared -o $@ $<

$(OUT)/titles/MicrosoftGame.config: tests/titles/MicrosoftGame.config | $(OUT)/titles
	cp $< $@


$(OUT) $(OUT)/titles $(OUT)/lib:
	mkdir -p $@

$(OUT)/jackbox.exe: loader/jackbox.c loader/jb_support.c loader/jb_loader.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ loader/jackbox.c loader/jb_support.c -lpsapi $(LDSTATIC) \
	  -Wl,--image-base,0x7ff700000000 -Wl,--dynamicbase -Wl,--high-entropy-va

$(OUT)/xgameruntime.dll: $(RUNTIME_SRC) runtime/xgameruntime/xgr.h runtime/xgameruntime/xgameruntime.def | $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ $(RUNTIME_SRC) runtime/xgameruntime/xgameruntime.def $(LDSTATIC)

# Import library for test titles. tests/titles/xgameruntime_title.def lists
# what titles import, including functions JackBox does not implement yet, so
# the diagnostics path can be exercised.
$(OUT)/lib/libxgameruntime_title.a: tests/titles/xgameruntime_title.def | $(OUT)/lib
	$(DLLTOOL) -d $< -l $@

titles: $(TESTS_C) $(TESTS_CXX) $(OUT)/titles/libtitledll.dll

$(OUT)/titles/libtitledll.dll: tests/titles/dll/titledll.c | $(OUT)/titles
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,--image-base,0x140000000 -Wl,--dynamicbase -Wl,--out-implib,$(OUT)/lib/libtitledll.a $(LDSTATIC)

$(OUT)/titles/%.exe: tests/titles/%.c $(OUT)/lib/libxgameruntime_title.a $(OUT)/titles/libtitledll.dll | $(OUT)/titles
	$(TCC) $(CFLAGS) -o $@ $< -L$(OUT)/lib -lxgameruntime_title -ltitledll -Wl,--dynamicbase

$(OUT)/titles/%.exe: tests/titles/%.cpp $(OUT)/lib/libxgameruntime_title.a | $(OUT)/titles
	$(TCXX) $(CFLAGS) -o $@ $< -static -Wl,--dynamicbase

test: all
	tests/run_titles.sh

clean:
	rm -rf build

.PHONY: all titles test clean
