# jev-jelly: src/core is the same everywhere; src/platform/<linux|windows> is the OS layer (see src/platform/plat.h).
#   make                     Linux (X11 + GLX)
#   make PLATFORM=windows    Windows (Win32 + WGL), cross-built with mingw-w64 or built natively in MSYS2
#   make test                core unit tests (no display needed)
# Linux needs the X11/GL dev headers (libx11-dev libxext-dev libgl-dev). Without root, scripts/fetch-headers.sh puts
# them in third_party/sysroot and this Makefile picks them up. cimgui (Dear ImGui C bindings, a git submodule) and
# the OpenGL3 backend are built into a static lib.
PLATFORM ?= linux
VERSION ?= $(shell cat VERSION 2>/dev/null)-dev
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS += -DJELLY_VERSION='"$(VERSION)"'
CXXFLAGS ?= -O2 -fno-exceptions -fno-rtti
IMGUI = third_party/cimgui
B = build/$(PLATFORM)

ifeq ($(PLATFORM),windows)
  # a local mingw-w64 unpacked into third_party/mingw (see scripts/fetch-mingw.sh), or one on PATH
  MINGW = $(firstword $(wildcard third_party/mingw/usr/bin/x86_64-w64-mingw32-gcc-posix))
  ifneq ($(MINGW),)
    CC = third_party/mingw/usr/bin/x86_64-w64-mingw32-gcc-posix
    CXX = third_party/mingw/usr/bin/x86_64-w64-mingw32-g++-posix
    AR = third_party/mingw/usr/bin/x86_64-w64-mingw32-ar
    WINDRES = third_party/mingw/usr/bin/x86_64-w64-mingw32-windres
  else ifneq ($(shell uname -s | grep -ci mingw),0)
    CC = gcc
    CXX = g++
    WINDRES = windres
  else
    CC = x86_64-w64-mingw32-gcc-posix
    CXX = x86_64-w64-mingw32-g++-posix
    AR = x86_64-w64-mingw32-ar
    WINDRES = x86_64-w64-mingw32-windres
  endif
  EXE = jelly.exe
  PLAT_SRC = src/platform/windows/plat_win.c
  PLAT_OBJ = $(B)/plat_win.o $(B)/jelly_res.o
  INC = -I$(IMGUI) -I$(IMGUI)/imgui
  # a GUI program (no console window), everything static: it runs on a clean Windows 10 / 11
  LDFLAGS ?= -static -mwindows
  LIBS = -lopengl32 -lgdi32 -luser32 -limm32 -lwinmm -lshell32 -lm
else
  EXE = jelly
  PLAT_SRC = src/platform/linux/plat_linux.c
  PLAT_OBJ = $(B)/plat_linux.o
  SYSINC = $(if $(wildcard third_party/sysroot/usr/include),-Ithird_party/sysroot/usr/include)
  INC = $(SYSINC) -I$(IMGUI) -I$(IMGUI)/imgui
  # the C++ runtime is linked in, so the binary only needs the system X11 / GL libraries
  LDFLAGS ?= -static-libstdc++ -static-libgcc
  LIBS = $(if $(wildcard third_party/lib),-Lthird_party/lib) -lX11 -lXext -lGL -lm -ldl -lpthread
endif

CORE = jelly options bubble calendar llm chat theme ui gl shot
CORE_OBJ = $(patsubst %,$(B)/%.o,$(CORE))
HDRS = $(wildcard src/core/*.h) src/platform/plat.h

IMGUI_SRC = $(IMGUI)/cimgui.cpp $(IMGUI)/imgui/imgui.cpp $(IMGUI)/imgui/imgui_draw.cpp $(IMGUI)/imgui/imgui_demo.cpp \
            $(IMGUI)/imgui/imgui_tables.cpp $(IMGUI)/imgui/imgui_widgets.cpp $(IMGUI)/imgui/backends/imgui_impl_opengl3.cpp
IMGUI_OBJ = $(patsubst %.cpp,$(B)/%.o,$(notdir $(IMGUI_SRC)))
vpath %.cpp $(IMGUI) $(IMGUI)/imgui $(IMGUI)/imgui/backends

$(EXE): $(CORE_OBJ) $(PLAT_OBJ) $(B)/libcimgui.a
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(B)/%.o: src/core/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(B)/plat_%.o: src/platform/$(PLATFORM)/plat_%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(B)/jelly_res.o: src/platform/windows/jelly.rc assets/jev-jelly.ico | $(B)
	$(WINDRES) -I assets -I src/platform/windows -o $@ $<

$(B)/%.o: %.cpp | $(B)
	$(CXX) $(CXXFLAGS) -I$(IMGUI)/imgui -I$(IMGUI)/imgui/backends -I$(IMGUI) $(SYSINC) \
	  -DIMGUI_IMPL_API='extern "C"' -c -o $@ $<

$(B)/libcimgui.a: $(IMGUI_OBJ)
	$(AR) rcs $@ $^

$(B):
	mkdir -p $(B)

# ---- tests: the core's pure logic (parsing, routing cues, calendar expansion, time zones) against the real
# platform layer, with no display and no network
TEST_EXE = $(B)/test_core$(if $(filter windows,$(PLATFORM)),.exe)
TEST_SRC = tests/test_main.c tests/test_llm.c tests/test_cal.c
test: $(TEST_EXE)
	$(if $(filter windows,$(PLATFORM)),$(if $(filter-out 0,$(shell uname -s | grep -ci mingw)),./$(TEST_EXE),@echo "built $(TEST_EXE): run it on Windows"),./$(TEST_EXE))
$(TEST_EXE): $(TEST_SRC) tests/check.h src/core/llm.c src/core/calendar.c $(PLAT_SRC) $(HDRS) | $(B)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable $(INC) -o $@ $(TEST_SRC) $(PLAT_SRC) \
	  $(if $(filter windows,$(PLATFORM)),src/core/gl.c -static $(LIBS),$(LIBS))

clean:
	rm -rf build jelly jelly.exe
.PHONY: clean test
