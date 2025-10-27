# Minimal RISC-V Kernel Makefile

# Kernel object files
OBJS = \
  kernel/entry.o \
  kernel/start.o \
  kernel/console.o \
  kernel/printf.o \
  kernel/uart.o \
  kernel/kalloc.o \
  kernel/string.o \
  kernel/vm.o \
  kernel/trampoline.o \
  kernel/trap.o \
  kernel/kernelvec.o \
  kernel/plic.o \
  kernel/kerneltest.o \
  kernel/main.o

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

# Tools
QEMU = qemu-system-riscv64
CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# Compiler flags for bare metal RISC-V kernel
CFLAGS = -Wall -Werror -Wno-unknown-attributes -O0 -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

# Linker flags
LDFLAGS = -z max-page-size=4096

# Default target
all: kernel/kernel

# Build kernel
kernel/kernel: $(OBJS) kernel/kernel.ld
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o kernel/kernel $(OBJS) 
	$(OBJDUMP) -S kernel/kernel > kernel/kernel.asm
	$(OBJDUMP) -t kernel/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > kernel/kernel.sym

# Compile C files
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile assembly files  
kernel/%.o: kernel/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Generate tags
tags: $(OBJS)
	etags kernel/*.S kernel/*.c

# Include dependency files
-include kernel/*.d

# Clean build artifacts
clean: 
	rm -f kernel/*.o kernel/*.d kernel/*.asm kernel/*.sym \
	kernel/kernel .gdbinit tags

# QEMU configuration
ifndef CPUS
CPUS := 1
endif

QEMUOPTS = -machine virt -bios none -kernel kernel/kernel -m 128M -smp $(CPUS) -nographic

# Run kernel in QEMU
qemu: kernel/kernel
	$(QEMU) $(QEMUOPTS)

# GDB debugging setup
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: kernel/kernel .gdbinit
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

.PHONY: all clean qemu qemu-gdb tags print-gdbport