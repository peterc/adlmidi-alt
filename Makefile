#CXX=i686-w64-mingw32-g++ -m64
#CXX=x86_64-w64-mingw32-g++ -m64 -static-libgcc -static-libstdc++

CXX     = g++
CXXLINK = $(CXX)

#DEBUG = -O0 -fno-inline -D_GLIBCXX_DEBUG -g -fstack-protector-all -fdata-sections -fsanitize=address

DEBUG=-O3 -ffast-math -g -march=native

# Enable OpenMP if the compiler supports it
OPENMP_SUPPORTED := $(shell echo 'int main(){}' | $(CXX) -fopenmp -x c++ - -o /dev/null 2>/dev/null && echo yes)
ifeq ($(OPENMP_SUPPORTED),yes)
DEBUG += -fopenmp
endif

#DEBUG += -fno-tree-vectorize

# -march=pentium -mno-sse -mno-sse2 -mno-sse3 -mmmx

CPPFLAGS += $(shell pkg-config --cflags sdl2)
LDLIBS   += $(shell pkg-config --libs sdl2)
#CPPFLAGS += $(SDL)

#LDLIBS += -lwinmm

CPPFLAGS += -std=c++17 -pedantic -Wall -Wextra

include make.rules

