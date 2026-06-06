RISCV_ARCH ?= rv64ima_zicsr_zifencei
RISCV_ABI ?= lp64
RISCV_MCMODEL ?= medany
CFLAGS += -O0 

RISCV_PREFIX ?= riscv64-unknown-elf-
ifeq (, $(shell command -v $(RISCV_PREFIX)gcc 2>/dev/null))
RISCV_PREFIX := riscv64-linux-gnu-
endif

RISCV_GCC     ?= $(RISCV_PREFIX)gcc
RISCV_AS      ?= $(RISCV_PREFIX)as
RISCV_GXX     ?= $(RISCV_PREFIX)g++
RISCV_OBJDUMP ?= $(RISCV_PREFIX)objdump
RISCV_GDB     ?= $(RISCV_PREFIX)gdb
RISCV_AR      ?= $(RISCV_PREFIX)ar
RISCV_OBJCOPY ?= $(RISCV_PREFIX)objcopy
RISCV_READELF ?= $(RISCV_PREFIX)readelf

.PHONY: all clean

BUILD_DIR ?= build
ELF := $(BUILD_DIR)/$(TARGET)
BINARY := $(ELF).bin
MEMORY_BIN := $(COMMON_DIR)/memory

# Sources
ASM_SRCS += $(COMMON_DIR)/start.S
override C_SRCS += $(wildcard *.c) $(wildcard $(COMMON_DIR)/lib/*.c)

LINKER_SCRIPT := $(COMMON_DIR)/link.lds

INCLUDES += -I$(COMMON_DIR)
INCLUDES += -I$(COMMON_DIR)/include

LDFLAGS += -T $(LINKER_SCRIPT) -nostartfiles -nostdlib -Wl,--gc-sections -Wl,--check-sections -lgcc

# Define objects (sort to remove duplicates)
ASM_OBJS := $(sort $(addprefix $(BUILD_DIR)/, $(notdir $(ASM_SRCS:.S=.o))))
C_OBJS := $(sort $(addprefix $(BUILD_DIR)/, $(notdir $(C_SRCS:.c=.o))))

# Handle source paths via VPATH
SRC_DIRS := $(sort $(dir $(ASM_SRCS) $(C_SRCS)))
vpath %.S $(SRC_DIRS)
vpath %.c $(SRC_DIRS)

LINK_OBJS += $(ASM_OBJS) $(C_OBJS)
LINK_DEPS += $(LINKER_SCRIPT)

# Objects to clean (everything in build dir)
CLEAN_DIRS += $(BUILD_DIR)

CFLAGS += -march=$(RISCV_ARCH)
CFLAGS += -mabi=$(RISCV_ABI)
CFLAGS += -mcmodel=$(RISCV_MCMODEL)  -O2 -ffreestanding -gdwarf

all: $(ELF)
	cp $(BINARY) $(MEMORY_BIN)
	make -C $(COMMON_DIR)/.. run

$(ELF): $(LINK_OBJS) $(LINK_DEPS) Makefile
	@mkdir -p $(dir $@)
	$(RISCV_GCC) $(CFLAGS) $(INCLUDES) $(LINK_OBJS) -o $@ $(LDFLAGS)
	$(RISCV_OBJCOPY) -O binary $@ $(BINARY)
	$(RISCV_OBJDUMP) -alDS -M no-aliases $@ > $(ELF).dump

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(RISCV_GCC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(RISCV_GCC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -rf $(CLEAN_DIRS)
