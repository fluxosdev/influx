# inFlux

inFlux is a small x86-64, Unix-like operating system with a preemptive,
multithreaded kernel and a macOS-style desktop. It boots with the Limine
bootloader, drives SATA (AHCI) and NVMe disks, reads and writes btrfs
volumes with its own filesystem driver, and browses the web (HTTP and HTTPS)
over its own TCP/IP stack. User programs see the Linux x86-64 system call
interface, so ordinary POSIX C code built against musl runs unchanged.

**This operating system was written by Claude**, Anthropic's AI model:
kernel, drivers, filesystems, window server, applications, shell and build
system. The user-space C library is musl and TLS is BearSSL (both vendored
in `third_party`).

## Features

### Kernel
- 64-bit kernel loaded by Limine (BIOS and UEFI), higher-half mapping
- GDT/TSS, full IDT with exception reporting, I/O APIC + local APIC timer
- Physical memory manager (bitmap), 4-level paging with per-process address
  spaces, bucket heap allocator
- Preemptive round-robin scheduler with kernel threads and ring-3 user
  threads, sleep queues, wait queues, mutexes, semaphores, FPU state switching
- SMP: every core Limine finds (up to 64) runs threads from a shared run
  queue, with per-CPU GDT/TSS, LAPIC timers and idle threads, IPIs to wake
  idle cores and TLB shootdowns; a worker pool (`smp_parallel_for`) spreads
  window composition and the blur over the cores. `cmdline: nosmp` in
  `limine.conf` keeps to one CPU
- **AVX required**: boot stops at the splash screen with an error on CPUs
  without AVX (Intel Sandy Bridge / AMD Bulldozer or newer). XSAVE/XSAVEOPT
  context switching of the full x87/SSE/AVX (and AVX-512) register state; AVX
  paths (AVX2 chosen at boot when present) for `memcpy`/`memset`, alpha
  compositing, glyph rendering and the blur; the OpenGL renderer, image decoder
  and all user programs are compiled with `-mavx`. The CPU's extensions are
  listed in Settings → About
- **Linux x86-64 system call ABI** (`syscall`/`sysret`, about 150 calls):
  files and directories (`openat` family, `getdents64`, `fcntl`, `pread`/`pwritev`,
  symbolic links), `poll`/`ppoll`/`select`, pipes, BSD sockets, `fork`
  (copy-on-write), `execve`, `wait4`, `clone` threads, futexes, `mmap`/
  `mprotect`/`munmap`/`brk`, signals, `clock_gettime`/`nanosleep`,
  `getrandom`, process groups and sessions. inFlux's own calls (desktop
  windows, menus, dialogs, notifications, process and disk listings) live at
  0x1000 and up
- **Virtual memory per process**: areas with their own protection, pages
  allocated on first touch, copy-on-write `fork` with shared-page reference
  counts. Every kernel access to user memory goes through fault-checked copies,
  so a bad pointer is `EFAULT`, never a kernel fault
- **Threads** with per-thread TLS (`FS` base), futexes (wait/wake/requeue,
  bitsets, timeouts) and `CLONE_CHILD_CLEARTID`, so musl's pthreads work
- **Signals**: Linux frame layout and `rt_sigreturn`, `SA_SIGINFO`,
  `SA_RESTART` (interrupted calls restart or return `EINTR`), `SA_ONSTACK`,
  masks, `sigsuspend`, faults delivered as `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE`,
  `SIGPIPE`, `SIGCHLD`, Ctrl+C as `SIGINT` to the terminal's foreground job
- **ELF loader**: static and position-independent executables, and
  dynamically linked ones through musl's dynamic linker (`PT_INTERP`,
  `/lib/ld-musl-x86_64.so.1`), with the System V initial stack and auxiliary
  vector. Segments are read straight from the file. `#!` interpreter scripts
  work, and execve needs an execute bit
- **POSIX files** for building software:
  - modes (umask, `chmod`) and nanosecond timestamps (`utimensat`)
  - hard links, `rename` that replaces its target in one step, and named pipes (`mkfifo`), on tmpfs and btrfs
  - `alarm`/`setitimer`
- Kernel CSPRNG (ChaCha20 seeded from RDSEED/RDRAND, timing jitter and
  interrupt timing): `getrandom`, `/dev/urandom`, TCP sequence numbers
- Timekeeping: a nanosecond monotonic clock from the invariant TSC, the wall
  clock kept by SNTP and written back to the RTC
- Kernel log (`dmesg`), and a **crash reporter**: a panic shows the message,
  a symbolized backtrace and the log tail on screen and, on an installed
  system, writes them straight to a reserved file on the EFI partition
  through the polled disk driver; the next boot saves the report to
  `~/Crash Reports` and says so
- **Processor power**: idle CPUs wait in the deepest C-state MWAIT offers
  (when the APIC timer keeps running there); Intel hardware P-states run in
  balanced mode, and SpeedStep processors (Haswell) get a load-based
  frequency governor

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
  nodes (`null`, `zero`, `urandom`, `console`, `kmsg`, ...), pipes, ttys with
  termios and **symbolic links** (tmpfs and btrfs)
- **ext2/ext3/ext4, read-only**: extents and classic block maps, 64-bit
  descriptors, hashed directories, symbolic links (Linux disks show up in
  Files)
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
  LVDS panels, TV-out and a second display keep the boot mode.
- **Intel Haswell driver** (HD Graphics 4600, and the rest of Gen7.5: HD
  4200/4400/5000, Iris 5100, Iris Pro 5200), modelled on Linux's `i915`:
  finds the pipe whose primary plane is the bootloader's framebuffer and the
  transcoder and DDI port behind it, then moves the scanout to two buffers in
  system memory mapped through the GTT for **vblank-synchronised page
  flipping**, with a 64x64 ARGB **hardware cursor**. The monitor's EDID is read
  over GMBUS (HDMI, DVI, VGA) or DisplayPort AUX for the mode list.
  **Runtime resolution changes** go through the panel fitter: the pipe keeps
  the timing the firmware lit (and the DisplayPort link it trained), and
  smaller modes are scaled up with the aspect ratio kept. Every other
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
- **Intel PRO/1000 driver** (`em0`): the 82540EM and 82574L (QEMU's `e1000`
  and `e1000e`) and the LAN controller of the Intel 8-series chipset with its
  **I217-LM/I217-V** PHY (and the I218-LM/V on the same platform). Legacy
  descriptor rings of 256 receive and 128 transmit buffers in host memory,
  MSI or the routed INTx line. On the chipset parts the PHY is shared with
  the Management Engine: MDIO access goes through the software flag, and a
  PHY left in SMBus or power-gated mode is brought back by toggling LANPHYPC,
  as Linux's `e1000e` does.
- **TCP/IP stack**: Ethernet, ARP (cache, request retries, packets parked
  until the next hop resolves), IPv4 with routing through a default gateway,
  ICMP echo (answers pings, and `ping`), UDP, a loopback interface
  (127.0.0.1, and the machine's own addresses), a DHCP client that keeps its
  lease (renewing at T1, rebinding at T2, starting over when it expires) and
  retries until a server answers, a DNS resolver with CNAME following and a
  TTL cache, and an **SNTP** client that sets the clock at boot and every four
  hours (`ntpdate` in the Terminal; Settings → Date & Time).
- **TCP**: active and passive open (listening sockets with an accept queue),
  MSS negotiation, in-order delivery with an
  out-of-order queue and duplicate ACKs for fast retransmit, RFC 6298
  retransmission timers with Karn's rule, slow start and congestion avoidance,
  zero-window probing, orderly close and TIME_WAIT. The advertised window is
  capped at what the adapter can buffer.
- **BSD sockets** for user programs: TCP and UDP, non-blocking connect,
  `poll`, `SO_ERROR`, timeouts, `socketpair`.
- **HTTP/1.1 client**: redirects, `Content-Length`, chunked and
  read-to-close bodies, GET and POST. In user programs it also speaks
  **HTTPS**: TLS 1.2 with BearSSL, certificates checked against Mozilla's
  root certificates and the system clock (`wget` in the Terminal).
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
- **System sounds**: notification and alert chimes, synthesized on the fly
  (Settings → Appearance turns them off)

### Web browser
- The Browser is an ordinary **user program** (`/bin/browser`, ring 3): it
  parses whatever the network sends, so a bug there can no longer take the
  kernel down. Its code is the same as before, built on `libui`, the kernel's
  drawing, font, widget and HTTP code compiled for user space.
- **HTTPS** with certificate validation; HTTP pages that redirect to HTTPS
  are followed.
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
- **Notifications**: banners in the top right corner (downloads, crash
  reports, low battery; `notify` in the Terminal)
- Battery level and charging state in the menu bar on laptops (ACPI
  batteries and AC adapter), and the display backlight through the ACPI
  video methods (brightness keys, `brightness` in the Terminal)
- Settings are saved (`~/.config/settings.conf`): appearance, wallpaper,
  clock, time zone, host name, volume, automatic time, sounds
- Light and dark appearance, eight accent colors, wallpaper presets
  (default: steel blue)
- Applications: **Browser**, **Files** (sidebar, icon/list views, history, new folder,
  rename, duplicate, move to Trash, Get Info), **Terminal**, **Text Editor**
  (selection, clipboard, undo, open/save), **Calculator**, **Task Manager**
  (live CPU/memory/threads, force quit), **System Settings**, **Audio Player**
- zsh-like shell: line editing, history (persisted to `~/.zsh_history`), Tab
  completion, aliases, environment variables, globbing, pipes, redirection,
  `&&`/`||`, background jobs, 60+ built-ins, and external programs from `/bin`
- User programs (static, musl): `hello`, `cat`, `ls`, `echo`, `primes`,
  `sleep`, `sysinfo`, `crash`, `notify`, `wget`, `glcube`, `browser`,
  `systest` (the system call checks), and `hello-dyn`, a dynamically linked one

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
make test                # host filesystem regressions + a headless boot that runs /bin/systest
make test SELFTEST_NET=1 # also DNS, TCP and HTTPS checks (needs the internet)
make test-fs             # the ext4 driver against a volume made by mkfs.ext4
make test-btrfs          # the POSIX file checks on a disk the kernel formats as btrfs
make system              # just build/influx.elf and build/initrd.tar (no xorriso needed)
```

### Developer tools (building inFlux on inFlux)

`docs/self-hosting.md` is the roadmap.

```sh
tools/toolchain/build-devtools.sh      # gcc, g++, binutils, make, BusyBox for inFlux -> build/devtools (20-60 min)
tools/toolchain/hermetic-build.sh      # build inFlux on Linux with only those tools on PATH
make DEVTOOLS=1                        # an ISO with the tools at /usr and BusyBox as /bin/sh
make test-devtools                     # boot it and run gcc, g++, sh and make -j4 on inFlux
```

`make test` boots the ISO with the Limine option `cmdline: selftest`: the kernel runs
`/bin/systest`, which checks the system call interface (memory, signals, fork/exec, threads,
files, symbolic links, pipes, poll, sockets over loopback, time) with its output on the
serial port, and powers off. The same runs in CI (`.github/workflows/ci.yml`).
`tools/qmp.py` drives a headless VM's desktop (screenshots, clicks, typing) over QMP.

The build compiles the kernel with the host GCC in freestanding mode, rasterises
the fonts and icons with the small host tools in `tools/`, packs `build/sysroot`
into a ustar initrd and produces a hybrid BIOS/UEFI ISO with Limine.

The emulated sound card plays through the host's PulseAudio or PipeWire
(`-audiodev pa`); use `make run QEMU_AUDIODEV=pipewire` for another QEMU audio
backend, or `make run QEMU_AUDIO=` to leave the card out.

inFlux needs an AVX-capable CPU. The `run` targets pass `-cpu max` (QEMU's
default emulated CPU has no AVX) and `run-kvm` passes `-cpu host`; when starting
QEMU by hand, pick a CPU model with AVX.

The `run` targets start two CPUs; use e.g. `make run QEMU_SMP="-smp 8"`, or
`make run QEMU_SMP=` for one.

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
- `make run` attaches an NE2000 to QEMU's user-mode network (`make run
  QEMU_NIC=e1000e` or `QEMU_NIC=e1000` for the Intel driver); the adapter gets
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
                    (radeon.c, radeon_kms.c), AtomBIOS interpreter (atom.c), EDID (edid.c),
                    Intel Haswell graphics (intel_gfx.c), NE2000, Intel PRO/1000 (e1000.c)
kernel/src/usb      USB core and hubs (usb.c), xHCI, EHCI, HID, mass storage
kernel/src/net      Ethernet/ARP, IPv4/ICMP, UDP, TCP, loopback, DHCP, DNS, SNTP, sockets, HTTP client
kernel/src/gl       kernel build of the OpenGL renderer and the OpenGL Demo app
gl                  shared software OpenGL implementation (gl.c / gl.h)
kernel/src/fs       VFS, tmpfs, initrd, tty, pipes, poll, devices, btrfs, FAT32, ext4 (read-only)
kernel/src/gfx      canvas primitives, fonts, images
kernel/src/ui       window server, theme, widgets, dialogs, app registry
kernel/src/apps     Files, Terminal, shell, Editor, Calculator, Task Manager, Settings, Installer
user/bin            user-mode programs (POSIX C, linked with musl)
user/lib            libinflux: the inFlux-specific system calls
user/ui             libui: the desktop toolkit for user programs (kernel sources + shims), TLS
user/apps           the Browser and wget, built on libui
tests               host filesystem regressions, the QEMU self-test runner, test volumes
tools               fontgen, imgconv, icon renderer, test-disk builder
assets              fonts, icons, logo
third_party         Limine, stb headers, musl, BearSSL, Mozilla CA certificates
```

## Limitations

- SMP: one shared run queue with no CPU affinity, every device interrupt is
  delivered to the boot CPU, and xAPIC only (no x2APIC, so no more than 255
  APIC IDs).
- USB: no UHCI/OHCI drivers (keyboards on USB 1.1 companion ports keep working
  through the BIOS's PS/2 emulation), isochronous transfers on xHCI only (USB
  audio does not work behind an EHCI controller), mass storage over Bulk-Only
  only (no UAS), one configuration per device.
- USB audio: playback only (no microphones; the audio core has no capture
  path), no MIDI, no webcams. Only QEMU's UAC1 device has been tested; UAC2
  devices and feedback endpoints follow the specification but are untested.
- Networking: IPv4 only, NE2000 and Intel PRO/1000 adapters only (the newer
  chipset LAN of the I218-LM/V on Haswell ultrabooks and the I219 is not
  supported: those need ultra-low-power exit and other quirks), no checksum or
  segmentation offload, no IP fragment reassembly, no AF_UNIX sockets other
  than `socketpair`.
- The browser has no JavaScript or style sheets, so modern sites render as
  unstyled documents. TLS is 1.2 (BearSSL has no TLS 1.3).
- Processes: `execve` from a multithreaded process fails with `EBUSY`; no
  job control stops (`SIGSTOP`/`SIGTSTP` are ignored); no hard links;
  `chmod`/`chown` are accepted but there are no users or permission checks
  yet; at most 64 descriptors and 64 user processes.
- ext4 is read-only and does not replay the journal; NTFS and exFAT are not
  supported. Symbolic links on btrfs follow Linux's layout but have not been
  checked with `btrfs check`.
- Power: no suspend (S3); battery, AC and backlight support follows the ACPI
  specification but has not been tested on a laptop (QEMU has none of them),
  and the SpeedStep governor has not run on real hardware either.
- btrfs: single-device volumes with CRC32C checksums only; no compression,
  snapshots, RAID or zoned support. Metadata is updated in place rather than
  copy-on-write, so an interrupted write can leave the volume inconsistent;
  the free space tree is kept consistent, or marked for rebuild if a block
  group uses the bitmap format.
- Apart from the Browser, the desktop applications (Files, Terminal and its
  shell, Editor, Settings, ...) still run as kernel threads.
- The Radeon driver was written from the Linux radeon register definitions
  and has not been run on real HD 4000 hardware yet; it never touches the 3D
  engine (that needs the R700 CP microcode and a command-stream backend), so
  OpenGL is always software-rendered. QEMU does not emulate an R700, so the
  software path is what you see under QEMU.
- The Intel Haswell driver has not been run on real hardware yet either (QEMU
  does not emulate it). It does not program display PLLs, DDI buffers or
  DisplayPort link training, so it cannot light a display the firmware left
  off or change the pipe timing: Settings offers the boot mode and smaller
  ones (scaled), and a larger mode can only be saved for the next boot with
  `display WIDTHxHEIGHT` in the Terminal. Only the lit pipe is driven; the
  render engine is not used.
- The I217 code path has not been tested on real hardware; the 82540EM and
  82574L paths are tested under QEMU.
- The hardware clock is assumed to be in UTC; adjust the offset in
  System Settings → Date & Time.

## Licenses

inFlux's own code is released under the MIT license. Third-party components keep
their licenses: Limine (BSD-2-Clause), stb (public domain/MIT), Inter and
Source Code Pro (SIL Open Font License), Kora icons (GPL-3.0), musl (MIT),
BearSSL (MIT), the Mozilla CA certificate bundle (MPL-2.0).
