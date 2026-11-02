# inFlux build system
# Targets: all (ISO), kernel, initrd, run, run-kvm, disk, clean

CC      := gcc
LD      := gcc
AS      := gcc
HOSTCC  := gcc
BUILD   := build
ISO     := $(BUILD)/influx.iso
KERNEL  := $(BUILD)/influx.elf
INITRD  := $(BUILD)/initrd.tar
LIMINE  := third_party/limine

KCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-lto -fno-pic -fno-pie -m64 -march=x86-64 -mabi=sysv \
           -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
           -mcmodel=kernel -mgeneral-regs-only -O2 -g \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -Wno-type-limits -Wno-missing-field-initializers -Wno-sign-compare -Wno-address-of-packed-member \
           -Ikernel/include -Ithird_party/limine -MMD -MP
# the OpenGL software renderer and its demo use SSE floating point (FPU state is saved per thread)
KCFLAGS_SSE := $(filter-out -mno-sse -mno-sse2 -mgeneral-regs-only -mno-mmx,$(KCFLAGS)) -msse -msse2 -mno-mmx -Igl
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 -Wl,--gc-sections \
            -T kernel/linker.ld -Wl,--build-id=none
KASFLAGS := -ffreestanding -m64

KSRC_C := $(shell find kernel/src -name '*.c' | sort)
KSRC_S := $(shell find kernel/src -name '*.S' | sort)
KOBJ   := $(patsubst kernel/src/%.c,$(BUILD)/kernel/%.o,$(KSRC_C)) \
          $(patsubst kernel/src/%.S,$(BUILD)/kernel/%.o,$(KSRC_S))

# ---- userland ----
UCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -m64 -mno-red-zone -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -Iuser/libc/include -Igl -Wno-misleading-indentation
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -Wl,--build-id=none \
            -T user/libc/user.ld
ULIBC_SRC := $(wildcard user/libc/*.c) $(wildcard user/libc/*.S)
ULIBC_OBJ := $(patsubst user/libc/%,$(BUILD)/user/libc/%.o,$(ULIBC_SRC))
UPROGS    := $(patsubst user/bin/%.c,$(BUILD)/sysroot/bin/%,$(wildcard user/bin/*.c))

# ---- assets ----
FONTGEN := $(BUILD)/tools/fontgen
IMGCONV := $(BUILD)/tools/imgconv
SYSROOT := $(BUILD)/sysroot

# name:ttf:size  -> sysroot/sys/fonts/name-size.fnt
FONT_SPECS := inter-regular:Inter-Regular:11 inter-regular:Inter-Regular:13 \
              inter-regular:Inter-Regular:15 inter-regular:Inter-Regular:24 \
              inter-regular:Inter-Regular:34 \
              inter-medium:Inter-Medium:11 inter-medium:Inter-Medium:13 \
              inter-semibold:Inter-SemiBold:13 inter-semibold:Inter-SemiBold:15 \
              inter-semibold:Inter-SemiBold:20 \
              inter-bold:Inter-Bold:13 inter-bold:Inter-Bold:22 \
              scp-regular:SourceCodePro-Regular:14 scp-bold:SourceCodePro-Bold:14
FONT_FILES := $(foreach s,$(FONT_SPECS),$(SYSROOT)/sys/fonts/$(word 1,$(subst :, ,$(s)))-$(word 3,$(subst :, ,$(s))).fnt)
ICON_PNGS  := $(wildcard assets/icons/png/*.png)
ICON_FILES := $(patsubst assets/icons/png/%.png,$(SYSROOT)/sys/icons/%.img,$(ICON_PNGS))

.PHONY: all kernel initrd run run-kvm run-nogui disk clean distclean tools

all: $(ISO)

tools: $(FONTGEN) $(IMGCONV)

$(FONTGEN): tools/fontgen.c third_party/stb/stb_truetype.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ $< -lm

$(IMGCONV): tools/imgconv.c third_party/stb/stb_image.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ $< -lm

# ---- kernel ----
kernel: $(KERNEL)

$(BUILD)/kernel/gl/%.o: kernel/src/gl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_SSE) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(AS) $(KASFLAGS) -c $< -o $@

$(KERNEL): $(KOBJ) kernel/linker.ld
	$(LD) $(KLDFLAGS) $(KOBJ) $(shell $(CC) -print-libgcc-file-name) -o $@

-include $(KOBJ:.o=.d)

# ---- userland ----
$(BUILD)/user/libc/%.c.o: user/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/libc/%.S.o: user/libc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KASFLAGS) -c $< -o $@

$(SYSROOT)/bin/%: user/bin/%.c $(ULIBC_OBJ) user/libc/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $< $(ULIBC_OBJ) -o $@

# ---- fonts / icons ----
define FONT_RULE
$(SYSROOT)/sys/fonts/$(1)-$(3).fnt: assets/fonts/$(2).ttf $(FONTGEN)
	@mkdir -p $$(dir $$@)
	$(FONTGEN) $$< $(3) $$@
endef
$(foreach s,$(FONT_SPECS),$(eval $(call FONT_RULE,$(word 1,$(subst :, ,$(s))),$(word 2,$(subst :, ,$(s))),$(word 3,$(subst :, ,$(s))))))

$(SYSROOT)/sys/icons/%.img: assets/icons/png/%.png $(IMGCONV)
	@mkdir -p $(dir $@)
	$(IMGCONV) $< $@

# ---- initrd ----
SKEL_FILES := $(shell find sysroot -type f 2>/dev/null)

initrd: $(INITRD)

$(INITRD): $(FONT_FILES) $(ICON_FILES) $(UPROGS) $(SKEL_FILES) $(KERNEL)
	@mkdir -p $(SYSROOT)/boot/limine
	cp -r sysroot/. $(SYSROOT)/
	cp $(KERNEL) $(SYSROOT)/boot/influx.elf
	cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTIA32.EFI $(LIMINE)/limine-bios.sys $(SYSROOT)/boot/limine/
	tar --format=ustar --owner=0 --group=0 --mtime='2026-01-01' -C $(SYSROOT) -cf $@ .

# ---- ISO ----
$(BUILD)/limine: $(LIMINE)/limine.c
	$(HOSTCC) -O2 -o $@ $< -Wno-unused-result

$(ISO): $(KERNEL) $(INITRD) limine.conf $(BUILD)/limine
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/boot/limine $(BUILD)/iso/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso/boot/influx.elf
	cp $(INITRD) $(BUILD)/iso/boot/initrd.tar
	cp limine.conf $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTIA32.EFI $(BUILD)/iso/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso -o $@ 2>/dev/null
	$(BUILD)/limine bios-install $@ 2>/dev/null

# ---- test disks (btrfs on SATA, btrfs on NVMe) ----
DISK_SATA := $(BUILD)/sata.img
DISK_NVME := $(BUILD)/nvme.img

DISK_INSTALL := $(BUILD)/install.img

disk: $(DISK_SATA) $(DISK_NVME) $(DISK_INSTALL)

$(DISK_INSTALL):
	truncate -s 1G $@

$(DISK_SATA): tools/mkdisk.sh
	tools/mkdisk.sh $@ "inFlux SATA" 256

$(DISK_NVME): tools/mkdisk.sh
	tools/mkdisk.sh $@ "inFlux NVMe" 256

QEMU      := qemu-system-x86_64
QEMU_ARGS := -M q35 -m 2G -cdrom $(ISO) -boot d -serial stdio \
             -drive id=sata0,file=$(DISK_SATA),format=raw,if=none \
             -device ahci,id=ahci -device ide-hd,drive=sata0,bus=ahci.0 \
             -drive id=nvme0,file=$(DISK_NVME),format=raw,if=none \
             -device nvme,serial=INFLUX01,drive=nvme0 \
             -drive id=inst,file=$(DISK_INSTALL),format=raw,if=none -device ide-hd,drive=inst,bus=ahci.1 \
             -vga std -display gtk,zoom-to-fit=on

run: $(ISO) disk
	$(QEMU) $(QEMU_ARGS)

run-kvm: $(ISO) disk
	$(QEMU) -enable-kvm -cpu host $(QEMU_ARGS)

# boot an installed system (after using the installer on build/install.img)
run-installed: $(DISK_INSTALL)
	$(QEMU) -M q35 -m 2G -serial stdio -vga std -display gtk,zoom-to-fit=on -drive file=$(DISK_INSTALL),format=raw,if=none,id=d -device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
run-installed-uefi: $(DISK_INSTALL)
	cp -n $(OVMF_VARS) $(BUILD)/ovmf_vars.fd || true
	$(QEMU) -M q35 -m 2G -serial stdio -vga std -display gtk,zoom-to-fit=on \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -drive file=$(DISK_INSTALL),format=raw,if=none,id=d -device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/user $(BUILD)/sysroot $(BUILD)/iso $(KERNEL) $(INITRD) $(ISO)

distclean:
	rm -rf $(BUILD)
