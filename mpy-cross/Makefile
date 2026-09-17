include ../py/mkenv.mk

# define main target
PROG = mpy-cross

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

# OS name, for simple autoconfig
UNAME_S := $(shell uname -s)

# include py core make definitions
include $(TOP)/py/py.mk

INC += -I.
INC += -I$(BUILD)
INC += -I$(TOP)

# compiler settings
CWARN = -Wall -Werror
CWARN += -Wextra -Wno-unused-parameter -Wpointer-arith
CFLAGS += $(INC) $(CWARN) -std=gnu99 $(COPT) $(CFLAGS_EXTRA)
CFLAGS += -fdata-sections -ffunction-sections -fno-asynchronous-unwind-tables

# Debugging/Optimization
ifdef DEBUG
CFLAGS += -g
COPT = -O0
else
COPT = -Os #-DNDEBUG
endif

# On OSX, 'gcc' is a symlink to clang unless a real gcc is installed.
# The unix port of MicroPython on OSX must be compiled with clang,
# while cross-compile ports require gcc, so we test here for OSX and
# if necessary override the value of 'CC' set in py/mkenv.mk
ifeq ($(UNAME_S),Darwin)
CC = clang
# Use clang syntax for map file
LDFLAGS_ARCH = -Wl,-map,$@.map -Wl,-dead_strip
else
# Use gcc syntax for map file. --cref is dropped because lld (LLVM-MinGW's
# linker) does not accept it. --gc-sections is enough on its own.
LDFLAGS_ARCH = -Wl,-Map=$@.map -Wl,--gc-sections
endif

# LLVM-MinGW's clang (22+) crashes with an SCCP-pass codegen bug on the
# inline-asm register-var reads in shared/runtime/gchelper_generic.c for the
# x86_64 host target. mpy-cross is a host tool where GC-scan speed does not
# matter, so fall back to the setjmp-based register capture, which sidesteps
# the crash and matches what MICROPY_GCREGS_SETJMP=1 selects at compile time.
COMPILER_TARGET_STR := $(shell $(CC) -dumpmachine)
COMPILER_VERSION := $(shell $(CC) --version 2>/dev/null | grep -oE 'clang version [0-9]+' | head -1 | grep -oE '[0-9]+')
ifneq (,$(findstring windows-gnu,$(COMPILER_TARGET_STR)))
ifneq (,$(filter-out 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21,$(COMPILER_VERSION)))
CFLAGS_EXTRA += -DMICROPY_GCREGS_SETJMP=1
endif
endif
LDFLAGS += $(LDFLAGS_MOD) $(LDFLAGS_ARCH) -lm $(LDFLAGS_EXTRA)

# source files
SRC_C = \
	main.c \
	gccollect.c \
	shared/runtime/gchelper_generic.c \

# Add fmode when compiling with mingw gcc, or with LLVM-MinGW (clang-22+),
# which reports "x86_64-w64-windows-gnu" instead of "*-mingw*".
COMPILER_TARGET := $(shell $(CC) -dumpmachine)
ifneq (,$(or $(findstring mingw,$(COMPILER_TARGET)),$(findstring windows-gnu,$(COMPILER_TARGET))))
	SRC_C += ports/windows/fmode.c
endif

OBJ = $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

include $(TOP)/py/mkrules.mk
