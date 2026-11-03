# inFlux

inFlux is a small x86-64, Unix-like operating system with a preemptive,
multithreaded kernel and a macOS-style desktop. It boots with the Limine
bootloader, drives SATA (AHCI) and NVMe disks, reads and writes btrfs
volumes with its own filesystem driver, and browses the web over its own
TCP/IP stack.

**This entire operating system was written by Claude**, Anthropic's AI model,
in a single session: kernel, drivers, filesystems, window server, applications,
shell, user-space C library and build system.

![inFlux desktop](docs/screenshot.png)

## Features

### Kernel
- 64-bit kernel loaded by Limine (BIOS and UEFI), higher-half mapping
- GDT/TSS, full IDT with exception reporting, I/O APIC + local APIC timer
- Physical memory manager (bitmap), 4-level paging with per-process address
  spaces, bucket heap allocator
- Preemptive round-robin scheduler with kernel threads and ring-3 user
  threads, sleep queues, wait queues, mutexes, semaphores, FPU state switching
- **AVX required**: boot stops at the splash screen with an error on CPUs
  without AVX (Intel Sandy Bridge / AMD Bulldozer or newer). XSAVE/XSAVEOPT
  context switching of the full x87/SSE/AVX (and AVX-512) register state; AVX
  paths (AVX2 chosen at boot when present) for `memcpy`/`memset`, alpha
  compositing, glyph rendering and the blur; the OpenGL renderer, image decoder
  and all user programs are compiled with `-mavx`. The CPU's extensions are
  listed in Settings → About
- Processes with file descriptor tables, working directories, exit status and
  `wait`; kernel-side killing of faulting user programs
- `syscall`/`sysret` fast system calls (31 calls: files, directories, process
  spawning, pipes, sleep, uname, sysinfo, process listing, block devices)
- Static ELF64 loader for user programs

### ACPI
- **AML interpreter** of its own: loads the DSDT and every SSDT into a
  namespace and runs module-level code; methods with arguments, locals and
  method-local objects; integers (32- or 64-bit by DSDT revision), strings,
  buffers, packages, references, `Index`/`RefOf`/`DerefOf`/`CondRefOf`,
  buffer fields, the arithmetic, logical, conversion and resource-template
  operators, `If`/`While` (with a runaway-loop timeout), `Load`/`LoadTable`,
  `_OSI` (answers like Windows), `\_OS`, `\_REV`
- Operation regions in SystemMemory, SystemIO, PCI_Config (bus found through
  `_BBN` and the bridges on the way), EmbeddedControl and CMOS; normal, index
  and bank fields with all access widths and update rules
- Brings the platform up the way firmware expects: enters ACPI mode, runs
  `_REG`, `_STA`/`_INI` over the device tree and `\_PIC(1)`
- **SCI**: the fixed power button and runtime GPEs (`_Lxx`/`_Exx`) handled by a
  worker thread; embedded controller (ECDT or PNP0C09) with `_Qxx` queries.
  Pressing the power button opens the Shut Down dialog
- **Power off through `\_S5`** (`\_PTS` first) and **reboot through the FADT
  reset register**, so both work on real hardware, not only in emulators
- **PCI interrupt routing** from `_PRT`, including link devices (`_CRS`,
  `_PRS`/`_SRS`) and bridge swizzling; drivers ask for MSI first and fall back
  to the routed INTx line
- `acpi` in the Terminal: tables and mode, `acpi tree [path]` dumps the
  namespace, `acpi eval _S5` evaluates any object, `acpi irq` lists routing

### Storage and filesystems
- PCI enumeration
- AHCI (SATA) driver with LBA48 DMA reads and writes
- NVMe driver (admin + I/O queue pair, PRP lists)
- Block device layer with GPT and MBR partition scanning (`/dev/sda1`,
  `/dev/nvme0n1p1`, ...)
- Virtual filesystem with a tmpfs root populated from a tar initrd, device
  nodes, pipes and ttys
- **btrfs read/write**: chunk mapping, B-tree search and modification with
  leaf/node splits, inline and regular extents, directory items and indexes,
  CRC32C checksums for metadata and data, extent-tree and free-space-tree
  bookkeeping. Volumes modified by inFlux pass `btrfs check` on Linux.

### USB
- **xHCI** (USB 3) host controller driver: BIOS hand-off, command and event
  rings, a transfer ring per endpoint, MSI or INTx interrupts with a polling
  fallback, USB 2 and USB 3 root ports; on Intel 7/8/9-series chipsets the
  ports are switched over from the EHCI controllers
- **EHCI** (USB 2.0) host controller driver: async schedule for control and
  bulk, periodic schedule for interrupt endpoints, split transactions through
  high-speed hubs; low/full-speed devices on root ports go to the companion
  controller
- USB core: enumeration, descriptors and strings, a hub thread that handles
  **hot plug and unplug**, USB 2.0 and USB 3 hubs (up to five tiers)
- **HID**: boot-protocol keyboards (software key repeat, Caps Lock LED),
  mice and tablets from their report descriptors (relative or absolute
  pointer, buttons, scroll wheel)
- Isochronous transfers and alternate settings (xHCI), used by **USB audio**
  (see Sound)
- **Mass storage** (Bulk-Only Transport, SCSI): USB sticks and disks become
  `/dev/sdX`, their partitions are mounted under `/mnt` and appear in Files;
  unplugging unmounts them
- `lsusb` (`-v` for interfaces and endpoints) shows the device tree

### Graphics
- **ATI Radeon R700 driver** (HD 4800/4700/4600/4300/4500 and the RS880 IGP),
  modelled on the Linux `radeon` driver: PCI probe, register and VRAM
  aperture mapping, VRAM size and MC framebuffer location, inspection of the
  scanout that the video BIOS (BIOS boot) or GOP (UEFI boot) configured,
  video BIOS discovery (ACPI VFCT table, C000:0000 shadow, IGP stolen
  memory or the PCI expansion ROM) and AtomBIOS parsing (bootup message,
  part number, default clocks), a 64x64
  ARGB hardware cursor and vblank-synchronised double-buffered page flipping
  of the primary surface. **Runtime mode setting** runs the video BIOS's own
  command tables through an **AtomBIOS interpreter** (a port of Linux's
  `atom.c`): the lit output is found through the BIOS object tables, the
  monitor's EDID is read over the DDC lines (bit-banged I2C) for the mode
  list, and the PLL, CRTC timing and DAC (VGA) or DIG/UNIPHY/LVTMA (DVI,
  HDMI in DVI mode) encoders are programmed in Linux's order. DisplayPort,
  LVDS panels, TV-out and a second display keep the boot mode. Every other
  adapter uses the **software fallback** (CPU compositing into the
  bootloader's linear framebuffer).
- **OpenGL**: fixed-function OpenGL 1.1 with a software rasteriser:
  immediate mode, vertex arrays (`glDrawArrays`/`glDrawElements`), display
  lists, modelview/projection/texture matrix stacks, eight lights with
  specular, spot lights, attenuation, two-sided lighting and colour material,
  fog, mipmapped textures (`gluBuild2DMipmaps`, trilinear filtering,
  repeat/clamp/mirrored wrapping, every texture environment), all blend
  factors, alpha and depth tests, scissor, colour/depth masks, polygon
  offset, point/line/fill polygon modes, wide lines and points,
  `glReadPixels` and the `glGet*` queries. Rasterisation is
  perspective-correct with a top-left fill rule and near-plane clipping.
  Not implemented: stencil/accumulation buffers, `glDrawPixels`/`glBitmap`,
  texgen, user clip planes, selection/feedback and `glPushAttrib`. The same
  implementation (`gl/gl.c`) is built into the kernel for desktop apps and
  into the user C library; `glGetString(GL_RENDERER)` names the adapter the
  frame ends up on. The desktop has an **OpenGL Demo** app and `/bin/glcube`
  renders from user space into a desktop window (`win_create`,
  `win_present`, `win_event` system calls).
- BIOS and UEFI boot both work through Limine's hybrid ISO; under BIOS the
  Radeon's VBIOS sets the mode through VBE and the driver takes over scanout
  programming from there.

![OpenGL demo](docs/opengl.png)

### Installer and display modes
- **Install inFlux** (Dock icon on the live system, or `install /dev/sdX` in the
  Terminal) writes a GPT with a 1 MB BIOS boot partition, a 64 MB FAT32 EFI
  System Partition (Limine for BIOS and UEFI, kernel, system image,
  `limine.conf`) and a btrfs root volume. The partitioning, FAT32 formatter,
  `mkfs.btrfs` and Limine's BIOS stage-2 install are all implemented in the
  kernel. The installed system boots on BIOS and UEFI firmware; its root
  volume holds the persistent home directory, mounted at `/home`.
- **Display resolution**: System Settings → Displays has a resolution
  dropdown with the common modes the display supports (on a Radeon R700, the
  monitor's own EDID modes). On QEMU, VirtualBox and Bochs (DISPI mode
  setting) and on the Radeon R700 (AtomBIOS) the mode changes immediately (the compositor
  re-lays out the desktop) and a "Keep this resolution?" banner reverts it
  after 15 seconds unless confirmed. A kept resolution is also written as
  `resolution:` into the installed system's `limine.conf`, so on real
  hardware it takes effect at the next boot through Limine (VBE on BIOS, GOP
  on UEFI). The `display` shell command does the same.

### Networking
- **NE2000 driver** for the PCI clones (Realtek RTL8029, which QEMU emulates
  as `ne2k_pci`, plus Winbond, VIA, Compex, Holtek and others) with a
  fallback probe of the classic ISA I/O bases. Frames move through the card's
  16 KiB buffer with remote DMA (`rep insw`/`rep outsw`). The adapter is
  polled by the network thread.
- **TCP/IP stack**: Ethernet, ARP (cache, request retries, packets parked
  until the next hop resolves), IPv4 with routing through a default gateway,
  ICMP echo (answers pings, and `ping`), UDP sockets, a DHCP client that
  configures the adapter at boot, and a DNS resolver with CNAME following and
  a TTL cache.
- **TCP**: active open with MSS negotiation, in-order delivery with an
  out-of-order queue and duplicate ACKs for fast retransmit, RFC 6298
  retransmission timers with Karn's rule, slow start and congestion avoidance,
  zero-window probing, orderly close and TIME_WAIT. The advertised window is
  capped at what the adapter can buffer.
- **HTTP/1.1 client**: redirects, `Content-Length`, chunked and
  read-to-close bodies, GET and POST.
- Shell commands: `ifconfig` (show, or set a static address), `dhcp`,
  `ping`, `nslookup`/`host`, `curl` (`-I`, `-i`, `-o file`), `arp`, `netstat`.

### Sound
- **Intel High Definition Audio** driver: controller reset, CORB/RIRB command
  rings (immediate commands as a fallback), codec discovery and widget graph
  parsing. Every analog output pin (speakers, headphones, line out) gets a
  path back to a DAC through mixers and selectors, which is powered up and
  unmuted, so sound comes out of all of them. HDMI/DisplayPort audio pins are
  not used yet.
- Playback from a 64 KiB cyclic DMA buffer at the source's sample rate when
  the codec supports it (8 to 192 kHz), otherwise resampled to 48 kHz; played
  regions are zeroed at once, so running dry gives silence, not a stutter loop
- **USB audio** (USB Audio Class 1 and 2, on xHCI): headsets, speakers and
  DACs play through isochronous transfers at a rate the device supports
  (the source's, else 48 kHz, else the device's own), as 16-, 24- or 32-bit
  samples in mono, stereo or multichannel layouts; adaptive and synchronous
  devices get the nominal rate, asynchronous ones follow their feedback
  endpoint. The device is unmuted at 0 dB and the master volume applies. A
  USB audio device plugged in becomes the output; unplugging it stops what is
  playing and the built-in card takes over again
- Plays 8/16/24/32-bit integer and 32-bit float PCM, mono to 8 channels
  (front left/right are used), with a master volume
- **Audio Player**: double-click a `.wav` file in Files; play/pause (Space),
  stop, seek (click the bar, or ← →), volume. In the Terminal: `play
  file.wav` (Ctrl+C stops) and `volume [0-100]`. `/dev/audio` takes raw
  48 kHz signed 16-bit stereo, e.g. `cat sound.raw > /dev/audio`
- No system sounds yet

### Web browser
- **Browser** (Dock): address bar that also searches (via wiby.me), back and
  forward, reload/stop, a progress bar, status bar with link targets and load
  times, View Source, Save Page As, and a start page.
- HTML parser that copes with real pages: implied end tags (`p`, `li`, `td`,
  `tr`, `option`, ...), character references, windows-1252 and UTF-8 text.
- Layout of headings, paragraphs, lists (bulleted, numbered, lettered, roman),
  definition lists, block quotes, preformatted text, rules, `<font>`, `<center>`,
  `align` and `bgcolor` attributes, and the inline `style` properties that
  matter most (colour, background, alignment, weight, `display: none`).
  Tables are sized from each cell's minimum and preferred widths, with
  `colspan`, `cellspacing`/`cellpadding`, borders, percentage and fixed widths
  and vertical alignment.
- Images (PNG, JPEG, GIF, BMP via stb_image) load after the page and reflow it
  as they arrive. Forms work: text, password, checkbox, radio, select,
  textarea and buttons, submitted with GET or POST.
- Opens `file://` paths (`.html` files from Files or `open page.html` in the
  Terminal), plain text and images. Other downloads go to `~/Downloads`.

### Desktop
- Software-composited window server with damage tracking, drop shadows,
  rounded windows, traffic-light controls, drag/resize/zoom/minimize
- macOS-like menu bar: system menu (About, Settings, Task Manager, Restart,
  Shut Down), per-application menus with keyboard shortcuts, date and time
- Dock with pinned apps, running indicators, hover tooltips and Trash
- Light and dark appearance, eight accent colors, wallpaper presets
  (default: steel blue)
- Applications: **Browser**, **Files** (sidebar, icon/list views, history, new folder,
  rename, duplicate, move to Trash, Get Info), **Terminal**, **Text Editor**
  (selection, clipboard, undo, open/save), **Calculator**, **Task Manager**
  (live CPU/memory/threads, force quit), **System Settings**, **Audio Player**
- zsh-like shell: line editing, history (persisted to `~/.zsh_history`), Tab
  completion, aliases, environment variables, globbing, pipes, redirection,
  `&&`/`||`, background jobs, 60+ built-ins, and external programs from `/bin`
- User programs: `hello`, `cat`, `ls`, `echo`, `primes`, `sleep`, `sysinfo`,
  `crash`

Icons come from the [Kora icon theme](https://github.com/bikass/kora), the UI
uses [Inter](https://fonts.google.com/specimen/Inter) and the terminal uses
[Source Code Pro](https://fonts.google.com/specimen/Source+Code+Pro).

## Building

Requirements (Debian/Ubuntu package names):

- `gcc`, `make`, `binutils`
- `xorriso` (ISO creation)
- `qemu-system-x86` (to run it)
- `btrfs-progs` and `util-linux` (`mkfs.btrfs`, `sfdisk`) to create the test disks

```sh
make            # builds build/influx.iso
make disk       # creates build/install.img (blank target for the installer)
make run        # boots in QEMU with a blank SATA install disk and an HD Audio card
make run-kvm    # same, with KVM acceleration
make run-installed       # boot the disk the installer wrote (build/install.img) on BIOS
make run-installed-uefi  # the same under OVMF UEFI
```

The build compiles the kernel with the host GCC in freestanding mode, rasterises
the fonts and icons with the small host tools in `tools/`, packs `build/sysroot`
into a ustar initrd and produces a hybrid BIOS/UEFI ISO with Limine.

The emulated sound card plays through the host's PulseAudio or PipeWire
(`-audiodev pa`); use `make run QEMU_AUDIODEV=pipewire` for another QEMU audio
backend, or `make run QEMU_AUDIO=` to leave the card out.

inFlux needs an AVX-capable CPU. The `run` targets pass `-cpu max` (QEMU's
default emulated CPU has no AVX) and `run-kvm` passes `-cpu host`; when starting
QEMU by hand, pick a CPU model with AVX.

Changing the SVG icon sources requires `python3` with `cairosvg`; run
`tools/render_icons.py` to regenerate the committed PNGs.

## Using it

- Click an icon in the Dock to start an application; double-click files in
  Files to open them.
- Keyboard shortcuts: `⌘Q` quit, `⌘W` close window, `⌘M` minimize, `⌘Tab` cycle
  windows, `⌘,` System Settings, `Ctrl+Alt+Delete` Task Manager. On a PC keyboard
  the Windows key is `⌘`; `Ctrl` works as an alias outside the Terminal.
- The btrfs volumes appear in the Files sidebar and under `/mnt`. Try
  `echo hi > /mnt/sda1/hello.txt` in the Terminal, then inspect `build/sata.img`
  on Linux (the filesystem starts 1 MiB into the image).
- `make run` adds an xHCI controller with a USB tablet. Attach a USB stick
  image with `make run USB_STICK=stick.img` (for example a file formatted by
  `mkfs.fat`); it is mounted under `/mnt`. `lsusb` lists the devices. For a
  USB sound card, add `QEMU_USB="-device qemu-xhci,id=xhci -device
  usb-tablet,bus=xhci.0 -device usb-audio,audiodev=snd0,bus=xhci.0"`.
- Type `help` in the Terminal for the list of shell built-ins; programs in
  `/bin` run in user mode. `lsgpu` shows the graphics adapter, `glcube &`
  starts the user-mode OpenGL program.
- `make run` attaches an NE2000 to QEMU's user-mode network; the adapter gets
  10.0.2.15 from QEMU's DHCP server at boot (`ifconfig` shows it). Open the
  Browser from the Dock: ⌘L focuses the address bar, ⌘R reloads, ⌘[ and ⌘]
  go back and forward, ⌘U shows the source. In the Terminal, try
  `ping 10.0.2.2`, `nslookup example.com` or `curl -I example.com`. On real
  hardware a DHCP server must answer within a few seconds, or set the address
  with `ifconfig ne0 192.168.1.50 netmask 255.255.255.0 gw 192.168.1.1`.

## Layout

```
kernel/src/arch     GDT, IDT, APIC, ACPI, PCI, timers, serial
kernel/src/mm       physical/virtual memory, heap
kernel/src/sched    scheduler, synchronisation, ELF loader, system calls
kernel/src/drivers  framebuffer, PS/2, AHCI, NVMe, block devices, GPU abstraction, Radeon R700
                    (radeon.c, radeon_kms.c), AtomBIOS interpreter (atom.c), EDID (edid.c), NE2000
kernel/src/usb      USB core and hubs (usb.c), xHCI, EHCI, HID, mass storage
kernel/src/net      Ethernet/ARP, IPv4/ICMP, UDP, TCP, DHCP, DNS, HTTP client
kernel/src/gl       kernel build of the OpenGL renderer and the OpenGL Demo app
gl                  shared software OpenGL implementation (gl.c / gl.h)
kernel/src/fs       VFS, tmpfs, initrd, tty, pipes, btrfs
kernel/src/gfx      canvas primitives, fonts, images
kernel/src/ui       window server, theme, widgets, dialogs, app registry
kernel/src/apps     Files, Terminal, shell, Editor, Calculator, Task Manager, Settings, Installer,
                    Browser (browser.c), HTML parser (html.c) and layout (layout.c)
user/libc           minimal C library for user programs
user/bin            user-mode programs
tools               fontgen, imgconv, icon renderer, test-disk builder
assets              fonts, icons, logo
third_party         Limine, stb headers
```

## Limitations

- Single CPU (other cores are left parked), no audio.
- USB: no UHCI/OHCI drivers (keyboards on USB 1.1 companion ports keep working
  through the BIOS's PS/2 emulation), isochronous transfers on xHCI only (USB
  audio does not work behind an EHCI controller), mass storage over Bulk-Only
  only (no UAS), one configuration per device.
- USB audio: playback only (no microphones; the audio core has no capture
  path), no MIDI, no webcams. Only QEMU's UAC1 device has been tested; UAC2
  devices and feedback endpoints follow the specification but are untested.
- Networking: IPv4 only, one NE2000 adapter type, TCP as a client only (no
  listening sockets), no IP fragment reassembly, DHCP leases are not renewed.
  The stack is used by the kernel and the desktop; user programs in `/bin`
  have no socket system calls yet.
- The browser speaks plain HTTP: no HTTPS (most of today's web), JavaScript
  or style sheets, so modern sites either redirect to HTTPS (an error page
  offers the FrogFind text proxy) or render as unstyled documents. Sites
  built for old computers, such as 68k.news, info.cern.ch, textfiles.com and
  wiby.me, work well.
- btrfs: single-device volumes with CRC32C checksums only; no compression,
  snapshots, RAID or zoned support. Metadata is updated in place rather than
  copy-on-write, so an interrupted write can leave the volume inconsistent;
  the free space tree is kept consistent, or marked for rebuild if a block
  group uses the bitmap format.
- The desktop applications run as kernel threads; only `/bin` programs run in
  ring 3.
- The Radeon driver was written from the Linux radeon register definitions
  and has not been run on real HD 4000 hardware yet; it never touches the 3D
  engine (that needs the R700 CP microcode and a command-stream backend), so
  OpenGL is always software-rendered. QEMU does not emulate an R700, so the
  software path is what you see under QEMU.
- The hardware clock is assumed to be in UTC; adjust the offset in
  System Settings → Date & Time.

## Licenses

inFlux's own code is released under the MIT license. Third-party components keep
their licenses: Limine (BSD-2-Clause), stb (public domain/MIT), Inter and
Source Code Pro (SIL Open Font License), Kora icons (GPL-3.0).
