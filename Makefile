# Needs the X11/GL dev headers (libx11-dev libxext-dev libgl-dev). Without root, scripts/fetch-headers.sh puts them in
# third_party/sysroot and this Makefile picks them up. cimgui (Dear ImGui C bindings, a git submodule) and the
# OpenGL3 backend are built into a static lib.
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS ?= -O2 -fno-exceptions -fno-rtti
# the C++ runtime is linked in, so the binary only needs the system X11 / GL libraries
LDFLAGS ?= -static-libstdc++ -static-libgcc
IMGUI = third_party/cimgui
SYSINC = $(if $(wildcard third_party/sysroot/usr/include),-Ithird_party/sysroot/usr/include)
INC = $(SYSINC) -I$(IMGUI) -I$(IMGUI)/imgui
LIBS = $(if $(wildcard third_party/lib),-Lthird_party/lib) -lX11 -lXext -lGL -lm -ldl

IMGUI_SRC = $(IMGUI)/cimgui.cpp $(IMGUI)/imgui/imgui.cpp $(IMGUI)/imgui/imgui_draw.cpp $(IMGUI)/imgui/imgui_demo.cpp \
            $(IMGUI)/imgui/imgui_tables.cpp $(IMGUI)/imgui/imgui_widgets.cpp $(IMGUI)/imgui/backends/imgui_impl_opengl3.cpp
IMGUI_OBJ = $(patsubst %.cpp,build/%.o,$(notdir $(IMGUI_SRC)))
vpath %.cpp $(IMGUI) $(IMGUI)/imgui $(IMGUI)/imgui/backends

jelly: build/jelly.o build/options.o build/libcimgui.a
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

build/%.o: src/%.c src/jelly.h | build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

build/%.o: %.cpp | build
	$(CXX) $(CXXFLAGS) -I$(IMGUI)/imgui -I$(IMGUI)/imgui/backends -I$(IMGUI) $(SYSINC) \
	  -DIMGUI_IMPL_API='extern "C"' -c -o $@ $<

build/libcimgui.a: $(IMGUI_OBJ)
	ar rcs $@ $^

build:
	mkdir -p build

clean:
	rm -rf build jelly
.PHONY: clean
