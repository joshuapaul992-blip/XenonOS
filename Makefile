# XenonOS build
#
#   make            same as "make image"
#   make image      bump the build number, build the kernel and build/xenonos.img
#   make kernel     build the kernel only (keeps the current build number)
#   make run        boot the image in QEMU (BIOS)       make run-uefi   (UEFI)
#   make test       run the automated tests on the current image
#   make clean      remove build output
#
# Every "make image" is a new build: XenonOS 0.0.1 (build N) -> (build N+1).

ifeq ($(origin CC),default)
CC := gcc
endif
LD      := ld
NASM    := nasm

BUILD   := build
KERNEL  := $(BUILD)/xnkrnl.elf
IMAGE   := $(BUILD)/xenonos.img
VERSION_H := $(BUILD)/include/xenon/version.h
LIMINE  := third_party/limine

CFLAGS := -std=gnu11 -O2 -g -pipe \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-pic -fno-pie -fno-omit-frame-pointer -fno-strict-aliasing \
    -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel \
    -Ikernel/include -I$(BUILD)/include -MMD -MP
NASMFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
    -z noexecstack --no-dynamic-linker -T kernel/linker.ld

KERNEL_C   := $(shell find kernel -name '*.c' | sort)
KERNEL_ASM := $(shell find kernel -name '*.asm' | sort)
KERNEL_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C)) $(patsubst %.asm,$(BUILD)/%.asm.o,$(KERNEL_ASM))

.PHONY: all image kernel run run-uefi run-q35 test clean fonts limine FORCE

all: image

image: limine
	@n=$$(tools/bump-build.sh) && echo "==> XenonOS $$(cat VERSION) (build $$n)"
	@$(MAKE) --no-print-directory $(IMAGE)

kernel: $(KERNEL)

limine:
	@tools/fetch-limine.sh

$(IMAGE): $(KERNEL) tools/mkimage.sh BUILD_NUMBER
	@tools/mkimage.sh $(KERNEL) $@

$(KERNEL): $(KERNEL_OBJ) kernel/linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(KERNEL_OBJ) -o $@

# version.h is checked on every build; it only changes after a bump.
$(VERSION_H): FORCE
	@tools/gen-version.sh $@

$(BUILD)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM    $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

# Regenerate the built-in console fonts from the PSF sources.
fonts:
	tools/psf2c.py third_party/fonts/terminus/Uni2-Terminus16.psf.gz KFontTerminus16 kernel/font/terminus16.c
	tools/psf2c.py third_party/fonts/terminus/Uni2-Terminus32x16.psf.gz KFontTerminus32 kernel/font/terminus32.c

run: $(IMAGE)
	tools/run-qemu.sh

run-uefi: $(IMAGE)
	tools/run-qemu.sh --uefi --q35

run-q35: $(IMAGE)
	tools/run-qemu.sh --q35

test:
	@tools/test.sh

clean:
	rm -rf ./$(BUILD)

-include $(KERNEL_OBJ:.o=.d)
