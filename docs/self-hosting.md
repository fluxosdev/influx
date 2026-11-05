# Self-hosting roadmap

Goal: build inFlux on inFlux. This file is the plan. Each phase has an exit test,
and no phase starts until the previous phase's test passes.

## What "self-hosting" means here

| Level | Meaning |
|-------|---------|
| **L1** | inFlux compiles its own kernel, userland and initrd from source, using a toolchain that was cross-compiled on Linux, and the result boots. |
| **L2** | The whole development loop runs on inFlux with no Linux machine: edit → `make` → install to the ESP → reboot. |
| **L3** | The toolchain (gcc, binutils, make, BusyBox) rebuilds itself on inFlux. |
| **L4** | Reproducible: the artifacts built on inFlux are byte-identical to the ones built on Linux. |

L1 and L2 are the real goal. L3 and L4 are stretch goals.

## Key decision: the toolchain

**Use a static GCC + binutils for `x86_64-linux-musl`, built with musl-cross-make.**

- The kernel implements the Linux x86-64 syscall ABI, so a static linux-musl binary
  should run unmodified. Nothing needs porting to a new OS. No custom target triple
  is needed either.
- The build depends on GCC-specific features: `-mcmodel=kernel`,
  `-mgeneral-regs-only`, AVX intrinsics in `simd.c`, GNU `as` syntax in the `.S`
  files, a GNU `ld` linker script, `libgcc` and `-MMD`. Matching the host compiler
  removes a whole class of problems.
- Alternatives:
  - **TCC** has no `-mcmodel=kernel`, no linker scripts and no AVX intrinsics.
    It is only good as a smoke test.
  - **Clang/LLVM** is 2–3× larger, and rebuilding it natively for L3 needs many GB
    of RAM. Not worth it.
- `uname -s` returns `inFlux`, so `config.guess` will fail. Pass
  `--build=x86_64-linux-musl --host=x86_64-linux-musl` explicitly. If that becomes
  tedious, patch `config.guess`/`config.sub`.

## Status (2026-11-05)

**Phases 0 and 1 are done.** A `DEVTOOLS=1` image runs gcc, g++, binutils, GNU make and
BusyBox on inFlux, including `make -j4` and incremental rebuilds (`make test-devtools`).
The whole system also builds on Linux with nothing on `PATH` but that bundle
(`tools/toolchain/hermetic-build.sh`), and the result passes the selftests.
**Next is Phase 2/3:** get the source onto inFlux and build musl, then the kernel.

The gaps found by the first survey, and where each stands:

| Gap | Status |
|-----|--------|
| No `#!` scripts (execve returned `ENOEXEC` for anything that isn't ELF) | **Done.** Interpreter plus optional argument, nested up to 4 deep (`kernel/src/sched/exec.c`). |
| No execute permission check | **Done.** Execve needs an execute bit (`EACCES`), and `access(X_OK)` follows the mode. Programs copied with the shell's `cp` or in Files keep their mode. |
| No user-space `/bin/sh` | **Done for `DEVTOOLS=1` images:** BusyBox ash, with `/bin/sh -> /usr/bin/busybox`. The Terminal still runs the kernel shell. |
| The Terminal only ran programs from `/bin`, with a fixed environment | **Done.** The kernel shell searches `$PATH` (default `/usr/bin:/bin`) and passes its variables as the environment. |
| No `utimensat`/`utimes`/`futimesat`/`utime` | **Done.** Only the modification time is kept; atime reads as mtime. |
| Timestamps in whole seconds | **Done.** Nanosecond mtime/ctime in the vnode and `stat`, kept on btrfs. A btrfs file's mtime is now the time of the write, not of the flush. |
| `chmod` was a no-op; `open`/`mkdir` ignored their mode | **Done.** Modes go through the umask and are stored (tmpfs, btrfs, initrd). There are still no owners, so the owner bits apply to everyone. |
| No hard links | **Done** on tmpfs and btrfs, with real `nlink`. `linkat`'s `AT_SYMLINK_FOLLOW` is ignored (the link itself gets the new name). |
| `rename` copied the file and refused non-empty directories | **Done.** It now happens in one step on tmpfs and btrfs, replaces the target as POSIX allows, and handles `RENAME_NOREPLACE`. Across filesystems `rename(2)` returns `EXDEV`, so `mv` copies; the shell and Files keep their copy fallback. |
| No `mkfifo`/`mknodat` | **Done.** FIFOs on tmpfs and btrfs, with POSIX blocking on open. `mknod` also makes regular files, but no device nodes. |
| No `setitimer`/`getitimer`/`alarm` | **Done.** `ITIMER_REAL` only, with 10 ms resolution (one kernel thread checks the deadlines). |
| `pselect6` ignored its signal mask (GNU make hung waiting for SIGCHLD) | **Done.** |
| `statfs` reported RAM for every path | **Done.** Disk volumes report their own size. |
| The initrd ignored symlinks and hard links | **Done.** They are unpacked, and directories get their modes. |
| Exec read the whole ELF into one kernel buffer | **Done.** It reads headers and segments straight from the file, 64 KiB at a time. |
| No data/page cache: every exec copies `cc1` again, and file `mmap` copies the whole file | **Open.** This is Phase 1 items 8–9. Correctness doesn't depend on it, but speed and memory under `-j` do. |
| `readdir` by index rescans the directory (O(n²) per listing) | **Open.** Cheap in practice: btrfs tree blocks are cached. |
| Only `/home` was persistent; `/` was rebuilt from the initrd | **Done** (Phase 4, first item). An installed system's btrfs volume is `/` (`kernel/src/fs/sysroot.c`), except `/dev`, `/tmp` and `/mnt`. The system image is synced onto it when it changes, using a manifest. The toolchain is a package that the installer puts on the disk, and `pkg install` updates it. `make test-persist` covers all of this. |
| The build used GNU tar's `--format`/`--owner`/`--mtime` | **Done.** `tools/mkinitrd.c` writes a sorted, reproducible ustar archive. `xorriso` is still needed for the ISO, but `make system` builds without it. |

Limits a build may hit:
- 64 user processes at once (`USER_PROCS_MAX`)
- 64 descriptors per process (`MAX_FDS`)
- execve from a multithreaded process returns `EBUSY`
- tmpfs keeps each file in one contiguous kernel allocation, capped at 256 MiB

## Phase 0: Prepare on Linux (no kernel changes)

The goal is to prove the build works with exactly the tools inFlux will have,
before anything runs on inFlux.

1. **Toolchain bundle script** (`tools/toolchain/build-devtools.sh`). It uses
   musl-cross-make to produce a native, static `x86_64-linux-musl` toolchain
   (pinned gcc 14.x, binutils 2.4x, musl 1.2.5 to match `third_party/musl`), with
   `--disable-nls --disable-multilib` and no LTO plugin. It then adds static
   GNU make 4.4.1 and BusyBox 1.36, all packed into `build/devtools.tar`.
   - Build C and C++ now, so L3 doesn't need a second bundle later.
2. **Make the Makefile BusyBox-clean:**
   - Replace the GNU `tar` call with `tools/mkinitrd.c`, a small deterministic
     ustar writer. This also makes the initrd reproducible.
   - Split `make system` (kernel + initrd) from the ISO step, so xorriso is only
     needed for the ISO.
   - Add `-ffile-prefix-map=$(CURDIR)=.` so build paths don't leak into the `-g`
     output (needed for L4).
   - Add a native-build mode, `ifeq ($(shell uname -s),inFlux)`, that sets the
     tool names and skips anything host-only.
3. **Hermetic check on Linux.** The bundle is static linux-musl, so it runs on the
   Linux host too. Use it to build with nothing else on `PATH`:
   `env -i PATH=$PWD/build/devtools/bin make SHELL=/path/to/busybox-sh system`.
   This catches every GNU-only assumption on Linux, where debugging is cheap.

**Exit test:** the hermetic Linux build produces a kernel and initrd that pass `make test`.

**Status: done.**
- `tools/toolchain/build-devtools.sh` builds the bundle: gcc 14.3.0 with C and C++, binutils 2.44, musl 1.2.5, GNU make 4.4.1 and BusyBox 1.36.1, 159 MB in total.
  - The compiler is relocatable: its sysroot is its install root, found through a `usr -> .` link.
  - It links dynamically by default, against `/lib/ld-musl-x86_64.so.1`, which inFlux has.
- `make BUILD=build/hermetic test` and `make BUILD=build/hermetic test-btrfs` pass on the output of `tools/toolchain/hermetic-build.sh`.
- Other changes to the build:
  - `-ffile-prefix-map` on everything, musl included.
  - The musl wrapper is forced with `--enable-wrapper=gcc`, because musl's configure skips it when the compiler already targets musl.
  - `HOST_LDFLAGS` is used for the host tools.

## Phase 1: Kernel features the toolchain needs

Add a systest check for each item in `user/bin/systest.c`.

**Correctness, in order:**
1. `#!` interpreter scripts in execve: interpreter plus one optional argument, with the rest of argv passed through.
2. `utimensat`, `futimens`, `utimes` (including `UTIME_NOW`/`UTIME_OMIT`), on both tmpfs and btrfs.
3. Nanosecond `mtime`/`ctime` in the vnode and in `stat`. btrfs inodes already store nanoseconds.
4. Real file modes: honour the `open`/`mkdir` mode and `umask`, and make `chmod`/`fchmod`/`fchmodat` store the mode. Permissions don't have to be enforced yet.
5. Hard links: `link`/`linkat` and real `nlink` on btrfs (inode refs) and tmpfs.
6. `mknodat` for FIFOs, plus `setitimer`/`alarm`. These are cheap, and they remove workarounds.
7. Out-of-memory: kill the process that faulted rather than panicking. `make -j` will hit this.

**Performance** (the build works without these, just slowly):

8. A page cache shared by file reads, `mmap` and exec. Exec then maps the ELF segments and faults pages in on demand, and concurrent `cc1` runs share pages.
9. Make `MAP_PRIVATE` file mappings copy-on-write from the page cache instead of copying the whole file up front.
10. Check `readdir`-by-index cost on large directories (`third_party/musl/src/*`, build directories). Avoid O(n²) scans.

**Exit test:** a systest run covering the new calls passes, and a smoke test with only
the `gcc` driver (no shell needed) works on inFlux: `gcc -static hello.c && ./a.out`
(run it on a `DEVTOOLS=1` ISO, see Phase 3). Try this smoke test early, even before
items 1–7 are done. It shakes out bugs in `fork`/`exec`/`wait`/pipes under real load.

**Status: items 1–7 done; items 8–10 (performance) open.**
- `make test` runs 101 checks, among them `#!`, modes, times, links, rename, FIFOs, timers and `pselect`.
- `make test-btrfs` runs the same POSIX file checks (`systest posix DIR`) on a blank disk that the kernel formats as btrfs.
- `make test-devtools` boots a `DEVTOOLS=1` image with 4 GiB of RAM and runs gcc, g++, sh, `make -j4`, an incremental make, `ar` and `nm` on inFlux.
- Item 7 needed no change: user page faults stop at a reserved margin of free memory and deliver SIGBUS, so a runaway build kills only itself.

## Phase 2: User-space base

- Install BusyBox as `/bin/sh` plus the applets the build uses: `sed grep awk find sort xargs tar gzip xz cp mv rm mkdir touch install cmp diff patch truncate env test vi`.
  - Build it static, and turn off applets that need missing syscalls (`mount` and similar).
- Install GNU make.
- Put the toolchain under `/usr` (`bin`, `lib`, `include`, `libexec`). Put `/usr/bin` on the default `PATH` in `exec.c`'s `default_env` and in the kernel shell.
  - The existing `/bin/{cat,ls,echo}` keep working.
- The Terminal keeps the kernel shell for now. Typing `sh` starts BusyBox ash, which needs the tty (job control, `SIGINT`, termios) to hold up.

**Exit test:** BusyBox's own testsuite (the subset of applets above) passes, and
`make -C` builds a small multi-file project with `-j4`.

**Status: partly done with Phase 1.**
- Done:
  - BusyBox (`/bin/sh` plus applets) and GNU make ship in `DEVTOOLS=1` images under `/usr`.
  - `/usr/bin` is first on the default `PATH`.
  - `make -C` builds a multi-file project with `-j4` (`make test-devtools`).
- Open:
  - BusyBox's own testsuite.
  - `sh` as an interactive shell in the Terminal.

## Phase 3: First native build (L1)

**Getting the tools and source in:**
- **Toolchain:** add a `make DEVTOOLS=1` option that unpacks `build/devtools.tar` into the initrd. This is simplest for now. The cost is about 100–150 MB more RAM.
- **Source in QEMU:** run `mkfs.btrfs --rootdir=<clean checkout> build/src.img` and attach it as a second disk. It mounts under `/mnt`.
- **Source on hardware:** `wget` a GitHub tarball into `/home` and unpack it with BusyBox `tar`.

**Build in steps**, from smallest to largest:
1. `gcc hello.c`
2. `kernel/src/lib/*.c`
3. musl: `configure` + `make`. This is the hardest shell workload (~1,700 objects).
4. BearSSL and libui
5. The host tools `tools/fontgen.c`, `tools/imgconv.c` and `third_party/limine/limine.c`
6. The kernel (linker script, `libgcc`)
7. `make system`

**Testing:**
- Use `make run-kvm` with `-m 4G` for this phase. TCG is too slow.
- After every build, run `btrfs check` on the source image from Linux. A full build is the heaviest churn btrfs will have seen: thousands of creates, renames and unlinks.

**Exit test:** a kernel and initrd built on inFlux, copied out of the disk image, boot
in QEMU and pass `make test`'s selftest.

## Phase 4: Close the development loop (L2)

- **Persistent system files:** done.
  - The installed volume is the root filesystem.
  - `DEVTOOLS=1` makes the toolchain a boot module. The installer copies it onto the disk (the 64 MB EFI partition couldn't hold it).
  - `pkg install FILE.tar [NAME]` installs or updates a package, with the same keep-your-changes rules as the system image.
- **`make install-local`:** copy `influx.elf` and `initrd.tar` to the ESP through the FAT32 driver. Keep the previous pair as a second Limine entry, "inFlux (previous)", so a bad kernel is one reboot away from recovery.
  - The crash reporter already writes to the ESP.
- **ISO on inFlux** (optional for L2): port xorriso (autotools with libisofs/libburn, heavy), or write `tools/mkiso.c` (ISO 9660 + El Torito + the hybrid layout Limine expects).
  - The Limine install step is already a small C tool.
- **Source control:**
  - *Interim:* `wget` tarballs plus BusyBox `patch`/`diff`.
  - *Proper:* port git. It needs zlib, and HTTPS needs curl with a TLS library. curl's BearSSL backend has been deprecated and removed in recent releases, so either pin an older curl or port mbedTLS.
- `make test` stays host-only, because it needs QEMU and bash. `/bin/systest` already runs natively.

**Exit test:** on an installed system (QEMU first, then the bare-metal second SSD),
edit a kernel string in the Text Editor, run `make system install-local`, reboot, and
see the change. No Linux involved.

## Phase 5: Toolchain rebuilds itself (L3, stretch)

Build these natively, in order:
1. GNU make
2. BusyBox
3. binutils. Configure-heavy, so a good test of `sh`/`sed`/`awk`.
4. GMP, MPFR and MPC
5. GCC: first with `--disable-bootstrap`, then the full three-stage bootstrap.
   - GCC is C++, so it needs the g++ shipped in Phase 0.
   - Expect several GB of RAM and disk and hours of runtime. That makes it the long-running stress test for the scheduler, memory and btrfs.

**Exit test:** the inFlux-built toolchain builds inFlux, and the Phase 3 exit test passes with it.

## Phase 6: Reproducibility and CI (L4, stretch)

- Add `make test-selfhost`: boot a `DEVTOOLS=1` ISO with the source disk under KVM, using the Limine `cmdline: selfbuild` (like `selftest`).
  - It runs the build through `/bin/sh`, prints SHA-256 hashes of the outputs on serial and powers off.
  - The test script then compares them with the host build's hashes.
- Add it to `.github/workflows/ci.yml` once it runs in reasonable time.

## Risks

- **Load bugs.** A full build is the largest workload inFlux has ever run: thousands of `fork`/`exec`/`wait` cycles, deep pipelines and `-j` parallelism. Scheduler races have only ever shown up under load, so expect Phase 3 to be mostly kernel bug-fixing.
- **btrfs write-path churn.** Keep running `btrfs check` after builds until it has been clean for a while.
- **Memory.** Until the page cache exists, every exec and every file `mmap` makes full copies, so `-j` multiplies memory use. Start with `-j1`/`-j2`.
- **AHCI timing.** It differs on real hardware (see the AHCI note in the driver), so a build that passes in QEMU still needs a bare-metal run.

## Critical path

Phase 0 bundle → `gcc hello.c` smoke test → `#!`, `/bin/sh`, `utimensat`, modes →
musl builds natively → kernel builds natively (**L1**) → persistent `/usr` and
`install-local` (**L2**).

The page cache can run in parallel with all of this. It isn't needed for correctness,
but it decides whether a full build takes minutes or hours.
