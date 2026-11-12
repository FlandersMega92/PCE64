BUILD_DIR = build
SOURCE_DIR = .
include $(N64_INST)/include/n64.mk

N64_ROM_TITLE = "PCE64"
# release libdragon (built with -DNDEBUG), see README
PCE_LIBDIR ?= $(CURDIR)/librel
ifneq ($(wildcard $(PCE_LIBDIR)/libdragon.a),)
N64_LIBDIR = $(PCE_LIBDIR)
endif

CORE = ../core
# libdragon's linker script with the emulator's hot code placed first (pce64.ld)
PCE_LDSCRIPT ?= $(CORE)/../n64/pce64.ld
N64_LDFLAGS := $(subst -Tn64.ld,-T$(PCE_LDSCRIPT),$(N64_LDFLAGS))
# link-time optimization as one unit (the driver does the LTO step)
CXX += -flto-partition=one
CFLAGS += -I$(CORE)
N64_CFLAGS += -O2 -flto -flto-partition=one -ftrivial-auto-var-init=uninitialized
LDFLAGS += -flto -flto-partition=one

PCE_STATS ?= 0
PCE_SCRIPT ?= 0
ifeq ($(PCE_STATS),1)
CFLAGS += -DPCE_STATS
endif
ifeq ($(PCE_SCRIPT),1)
CFLAGS += -DPCE_SCRIPT -I$(BUILD_DIR)
endif
CFLAGS += $(PCE_EXTRA)

PCE_JIT ?= 1
ifeq ($(PCE_JIT),1)
CFLAGS += -DPCE_JIT
endif
MAIN ?= main.c
ifeq ($(MAIN),main.c)
CFLAGS += -DJIT_DYNAMIC
endif
sources = $(MAIN) $(CORE)/cpu.c $(CORE)/cpu_step.c $(CORE)/cpu_ramrun.c $(CORE)/jit.c $(CORE)/pce.c $(CORE)/vdc.c $(CORE)/psg.c
objects = $(addprefix $(BUILD_DIR)/,$(subst ../,ext/,$(sources:.c=.o)))

all: pce64.z64

$(BUILD_DIR)/ext/%.o: ../%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/pce64.elf: $(objects)

clean:
	rm -rf $(BUILD_DIR) pce64.z64

.PHONY: all clean
