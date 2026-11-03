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
# Code that may use vector registers: the OpenGL software renderer and its demo,
# the image decoder and the SIMD kernels. inFlux requires AVX (kmain halts at the
# boot splash without it), so these assume it; vector state is saved per thread.
KCFLAGS_SIMD := $(filter-out -mno-sse -mno-sse2 -mgeneral-regs-only -mno-mmx,$(KCFLAGS)) -mavx -mno-mmx -Igl
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 -Wl,--gc-sections \
            -T kernel/linker.ld -Wl,--build-id=none
KASFLAGS := -ffreestanding -m64

KSRC_C := $(shell find kernel/src -name '*.c' | sort)
KSRC_S := $(shell find kernel/src -name '*.S' | sort)
KOBJ   := $(patsubst kernel/src/%.c,$(BUILD)/kernel/%.o,$(KSRC_C)) \
          $(patsubst kernel/src/%.S,$(BUILD)/kernel/%.o,$(KSRC_S))

# ---- userland ----
UCFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -m64 -mavx -mno-red-zone -O2 -g -Wall -Wextra -Wno-unused-parameter \
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
              scp-regular:SourceCodePro-Regular:14 scp-bold:SourceCodePro-Bold:14 \
              inter-regular:Inter-Regular:18 inter-regular:Inter-Regular:22 \
              inter-regular:Inter-Regular:28 inter-bold:Inter-Bold:11 \
              inter-bold:Inter-Bold:15 inter-bold:Inter-Bold:18 inter-bold:Inter-Bold:28
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
	$(CC) $(KCFLAGS_SIMD) -c $< -o $@

# the browser's image decoder (stb_image) also gets AVX
$(BUILD)/kernel/gfx/imgdecode.o: kernel/src/gfx/imgdecode.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_SIMD) -Wno-unused-but-set-variable -Wno-implicit-fallthrough -c $< -o $@

# SIMD kernels (memcpy/memset, compositing, blur): AVX, with AVX2 picked at run
# time; callers only enter them where simd_ok() says vector registers are safe
$(BUILD)/kernel/lib/simd.o: kernel/src/lib/simd.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_SIMD) -fno-tree-loop-distribute-patterns -c $< -o $@

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

# ---- install target disk ----
DISK_INSTALL := $(BUILD)/install.img

disk: $(DISK_INSTALL)

$(DISK_INSTALL):
	truncate -s 1G $@

QEMU      := qemu-system-x86_64
# A terminal inside snap-packaged VS Code inherits the snap's GTK/GIO environment
# (its own GSettings schemas, GTK modules, locale path); the host's GTK then aborts
# on a schema mismatch. Hand QEMU the host's environment back instead.
ifdef XDG_DATA_DIRS_VSCODE_SNAP_ORIG
QEMU := env -u GSETTINGS_SCHEMA_DIR -u GTK_PATH -u GTK_EXE_PREFIX -u GTK_IM_MODULE_FILE \
        -u GIO_MODULE_DIR -u GDK_PIXBUF_MODULE_FILE -u GDK_PIXBUF_MODULEDIR -u LOCPATH \
        XDG_DATA_DIRS='$(XDG_DATA_DIRS_VSCODE_SNAP_ORIG)' $(QEMU)
endif
# inFlux requires AVX, which TCG's default CPU model lacks; 'max' has AVX and AVX2
QEMU_CPU  := -cpu max
# processors; e.g. make run QEMU_SMP="-smp 8", or QEMU_SMP= for one
QEMU_SMP  ?= -smp 2
# NE2000 (RTL8029) on QEMU's user-mode network: DHCP gives 10.0.2.15, the host is reachable as 10.0.2.2.
# QEMU_NIC=e1000 (82540EM) or QEMU_NIC=e1000e (82574L) uses the Intel PRO/1000 driver instead
QEMU_NIC  ?= ne2k_pci
QEMU_NET  := -netdev user,id=net0 -device $(QEMU_NIC),netdev=net0
# Intel HD Audio with a line-out codec, played through the host's PulseAudio/PipeWire;
# override with e.g. QEMU_AUDIODEV=pipewire, or QEMU_AUDIO= for no sound card
QEMU_AUDIODEV ?= pa
QEMU_AUDIO ?= -audiodev $(QEMU_AUDIODEV),id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0
# xHCI with a USB tablet: an absolute pointer, so the window needs no mouse grab.
# Plug in a USB stick image with e.g. make run USB_STICK=stick.img, or set QEMU_USB=
# for no USB at all
QEMU_STICK = -drive id=stick,file=$(USB_STICK),format=raw,if=none -device usb-storage,bus=xhci.0,drive=stick
QEMU_USB ?= -device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0 $(if $(USB_STICK),$(QEMU_STICK))
# the live CD gets bootindex=0 so it boots ahead of an installed system on install.img
QEMU_ARGS := -M q35 -m 2G $(QEMU_SMP) -serial stdio \
             -drive id=cd0,file=$(ISO),format=raw,media=cdrom,readonly=on,if=none \
             -device ide-cd,drive=cd0,bus=ide.2,bootindex=0 \
             -device ahci,id=ahci \
             -drive id=inst,file=$(DISK_INSTALL),format=raw,if=none -device ide-hd,drive=inst,bus=ahci.0,bootindex=1 \
             $(QEMU_NET) $(QEMU_AUDIO) $(QEMU_USB) -vga std -display gtk,zoom-to-fit=on

run: $(ISO) disk
	$(QEMU) $(QEMU_CPU) $(QEMU_ARGS)

run-kvm: $(ISO) disk
	$(QEMU) -enable-kvm -cpu host $(QEMU_ARGS)

# boot an installed system (after using the installer on build/install.img)
run-installed: $(DISK_INSTALL)
	$(QEMU) $(QEMU_CPU) -M q35 -m 2G $(QEMU_SMP) -serial stdio $(QEMU_NET) $(QEMU_AUDIO) $(QEMU_USB) -vga std -display gtk,zoom-to-fit=on -drive file=$(DISK_INSTALL),format=raw,if=none,id=d -device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
run-installed-uefi: $(DISK_INSTALL)
	cp -n $(OVMF_VARS) $(BUILD)/ovmf_vars.fd || true
	$(QEMU) $(QEMU_CPU) -M q35 -m 2G $(QEMU_SMP) -serial stdio $(QEMU_NET) $(QEMU_AUDIO) $(QEMU_USB) -vga std -display gtk,zoom-to-fit=on \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -drive file=$(DISK_INSTALL),format=raw,if=none,id=d -device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/user $(BUILD)/sysroot $(BUILD)/iso $(KERNEL) $(INITRD) $(ISO)

distclean:
	rm -rf $(BUILD)
