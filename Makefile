TARGET = tanx
SRC = tanx.cpp
DEPS = $(SRC) olcPixelGameEngine.h miniaudio.h

# =============================================================================
# Platform detection
# Windows: run from a MinGW/MSYS2 shell (or `mingw32-make`) — $(OS) is set
#          to Windows_NT by the environment in both cmd.exe and MSYS2.
# macOS:   detected via `uname -s` == Darwin, libpng path found via Homebrew.
# Linux:   detected via `uname -s` == Linux.
# =============================================================================

ifeq ($(OS),Windows_NT)
    CXX = g++
    CXXFLAGS = -std=c++17 -Wall -O2
    LDFLAGS = -luser32 -lgdi32 -lopengl32 -lgdiplus -lshlwapi -ldwmapi -lstdc++fs
    LIBS =
    TARGET := tanx.exe
else
    UNAME_S := $(shell uname -s)

    ifeq ($(UNAME_S),Darwin)
        LIBPNG_PREFIX := $(shell brew --prefix libpng 2>/dev/null)
        CXX = clang++
        CXXFLAGS = -std=c++17 -Wall -O2 -mmacosx-version-min=10.15 -Wno-deprecated-declarations
        CXXFLAGS += -I$(LIBPNG_PREFIX)/include
        LDFLAGS = -framework OpenGL -framework GLUT -framework Carbon
        LDFLAGS += -L$(LIBPNG_PREFIX)/lib
        LIBS = -lpng
    endif

    ifeq ($(UNAME_S),Linux)
        CXX = g++
        CXXFLAGS = -std=c++17 -Wall -O2
        LDFLAGS = -lX11 -lGL -lpthread -ldl -lstdc++fs
        LIBS = -lpng
    endif
endif

all: $(TARGET)

$(TARGET): $(DEPS)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LIBS)

clean:
	rm -f tanx tanx.exe

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
