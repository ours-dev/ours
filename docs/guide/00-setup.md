# Setting up the build tools

Before anything in this guide builds, the checkout needs a directory of tools, `prebuilt/`: the compiler, `gn`, `ninja`, QEMU and GRUB. Git ignores that directory, and the tree has no installer for it. This chapter says exactly what the build reads from it, what each piece is, where each piece of the reference machine came from, and which parts nothing in the tree can yet reproduce.

It describes the one machine the project is built on, and says where it has not been checked. Read the last section before you start.

## What you will be able to do

- say which tools the build needs, and where each one has to be;
- assemble `prebuilt/` for a macOS host on Apple silicon, from the sources named here;
- check that it is complete before you run the first build.

## How the build finds its tools

The build never looks for a tool on your `PATH`. It names fixed paths under `prebuilt/` in the checkout, and the scripts that boot an image do the same. A checkout under `.worktrees/` links its `prebuilt/` to the main checkout's (`scripts/new_worktree.py`), so one `prebuilt/` serves every worktree.

Three rules follow from how the repository is run:

- Toolchains come from official upstream releases, unpacked into `prebuilt/`.
- Nothing is installed into a system directory. QEMU and GRUB are extracted from their packages and run from `prebuilt/`; they are never installed.
- Rebuilds after a toolchain update are keyed on a small version stamp kept next to each toolchain (see "The version stamps").

The reference machine is a Mac with Apple silicon. The compiler directory the build selects depends on the host: `build/config/compiler/clang.gni` uses `prebuilt/clang/llvm-23` on macOS and `prebuilt/clang/llvm-22` anywhere else, where `llvm-22` is a set of links into the LLVM 22 that `docker/dev.dockerfile` installs in the development container. That container installs LLVM and a few packages, and nothing else: `gn`, `ninja`, QEMU and GRUB would still have to be provided. This chapter has not been checked on Linux, and does not describe it further.

## What `prebuilt/` must hold

The versions are those on the reference machine, read from the tools themselves.

| Path under `prebuilt/` | What it is | Version | Read by |
|---|---|---|---|
| `clang/llvm-23/` | Clang and LLVM for macOS on arm64: `bin/` (`clang`, `clang++`, `ld.lld`, `llvm-ar`, `llvm-strip`, `llvm-readelf`, `llvm-objcopy`, `clang-format`, `clang-tidy` and the rest), the C++ library's headers in `include/c++/v1`, and the compiler runtime in `lib/clang/23/lib/` | 23.1.0-rc3 | `build/config/compiler/clang.gni`, `tools/set.py` |
| `clang/llvm-23/lib/clang/23/lib/x86_64-unknown-none-elf/libclang_rt.builtins.a` | The compiler runtime the x86-64 kernel links; one file, copied in by hand | from the 23.1.0-rc3 release | `build/config/compiler/clang.gni`, the kernel's link |
| `gn/gn` | The build generator | 2530 (`e0a6ab04a113`) | every script that builds |
| `ninja/ninja` | The build tool | 1.13.2 | every script that builds |
| `qemu/` | QEMU and the libraries it needs: `bin/qemu-system-x86_64`, `lib/`, `share/qemu/` | 11.1.1 | `scripts/qemu_boot.py` |
| `grub/` | GRUB for x86-64 and what it needs: `bin/x86_64-elf-grub-mkimage`, `lib/x86_64-elf/grub/x86_64-efi/`, and the `mtools` that make the boot disk | 2.12 | `scripts/qemu_boot.py` |
| `gcc/mac-arm64/.versions/gcc.cipd_version` | A version stamp and nothing else | stamp `gcc-15.2.1-elf` | `build/config/compiler/gcc.gni` |
| `ccache/ccache` | The compiler cache; optional | pinned by `scripts/fetch_ccache.py` | `tools/set.py`, `build/toolchain/compiler_launcher.gni` |

`prebuilt/` on the reference machine also holds `clang/llvm-22` (links into the container, broken on a Mac) and `rust/stable` (a Rust toolchain). The build on a Mac needs neither: nothing under `kernel/`, `system/`, `examples/` or `benches/` defines a Rust target, and `tools/set.py` skips a `clang` directory that has no `include/c++/v1` beside it.

## Getting each piece

### LLVM

Take the official LLVM release for the version in the table, `llvmorg-23.1.0-rc3`, from the LLVM project's release page on GitHub (`https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.0-rc3`). The page lists the archive for macOS on arm64 as `LLVM-23.1.0-rc3-macOS-ARM64.tar.xz` (there is a `.tar.zst` of it too). Unpack it into `prebuilt/clang/llvm-23/`, so that `prebuilt/clang/llvm-23/bin/clang` exists; list the archive with `tar -tf` first to see how many leading directories to strip (`--strip-components=1` if there is one).

The kernel also needs the compiler runtime for its own target, `x86_64-unknown-none-elf`, which a release does not hold under that name. The tree's note on how the reference machine got it is in `prebuilt/clang/llvm-23/lib/clang/23/lib/x86_64-unknown-none-elf/README.md`. In short, take `lib/clang/23/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a` out of the same release's `LLVM-23.1.0-rc3-Linux-X64.tar.xz` (also listed on the page), and copy it to `prebuilt/clang/llvm-23/lib/clang/23/lib/x86_64-unknown-none-elf/libclang_rt.builtins.a`. The note says why that is sound: the integer helpers in it are architecture-specific and independent of the operating system, and the objects are plain 64-bit x86 ELF.

Then write the version stamp, `prebuilt/clang/llvm-23/.versions/clang.cipd_version`:

```json
{
  "instance_id": "llvm-23.1.0-rc3"
}
```

### gn and ninja

Both are single executables. The reference copies were fetched on 2026-08-24, without the `cipd` client, from the package server of Chromium's infrastructure, which serves a package over plain HTTPS at `https://chrome-infra-packages.appspot.com/dl/<package>/+/<version>` (the slash before the version is required):

```bash
curl -sL "https://chrome-infra-packages.appspot.com/dl/gn/gn/mac-arm64/+/latest" -o gn.zip
curl -sL "https://chrome-infra-packages.appspot.com/dl/infra/3pp/tools/ninja/mac-arm64/+/latest" -o ninja.zip
```

Each zip holds the executable and a `.cipdpkg/` directory. Copy the executable to `prebuilt/gn/gn` and `prebuilt/ninja/ninja` and make it executable with `chmod +x`. These two commands worked that day, and were not run again while this chapter was written. `latest` moves, so the versions you get can be newer than the table's.

### QEMU and GRUB

The reference copies are Homebrew packages (bottles), extracted into `prebuilt/qemu/` and `prebuilt/grub/` and never installed. Each directory keeps the formula files of what it holds in `.brew/`. A package's programs record the absolute path `/opt/homebrew/lib` for their libraries, which does not exist where they are extracted; `scripts/qemu_boot.py` sets `DYLD_FALLBACK_LIBRARY_PATH` to the extracted `lib/` directories, which is what the system's loader looks in when a recorded path is missing, so no program needs rewriting.

Homebrew can download a bottle without installing it, into a cache you choose, and a bottle is an archive of `<formula>/<version>/...`. Extract each one with its first two directories stripped, into the prefix, so that the files of all of them merge:

```bash
export HOMEBREW_CACHE=$PWD/brew-cache HOMEBREW_NO_AUTO_UPDATE=1
brew fetch --deps qemu
mkdir -p prebuilt/qemu
tar -xzf "$HOMEBREW_CACHE/qemu--<version>" --strip-components=2 -C prebuilt/qemu
```

`brew fetch --deps` prints the path of each bottle it fetched; repeat the `tar` line for each of them, `qemu` and every dependency. For GRUB, do the same with the formula `x86_64-elf-grub` into `prebuilt/grub/`, and with two more formulas that the GRUB formula does not depend on and the reference `prebuilt/grub/` holds: `mtools`, whose `mformat` and `mcopy` `scripts/qemu_boot.py` runs to make the boot disk, and `xorriso`. Delete the cache directory afterwards if you do not want it.

What was run while this chapter was written: `brew fetch x86_64-elf-grub` with the cache set as above downloaded the bottle (`x86_64-elf-grub--2.12`, for the macOS release of the writer's machine, which Homebrew chooses), and extracting it with `--strip-components=2` produced the same layout as the reference `prebuilt/grub/` (`bin/x86_64-elf-grub-mkimage`, `lib/x86_64-elf/grub/x86_64-efi/`, `.brew/x86_64-elf-grub.rb`). What differs is the content: the reference copies are older builds of the same upstream versions, so file sizes differ, and Homebrew's dependency lists have moved on since (GRUB now needs `brotli`; QEMU now needs `openssl@4` and `ca-certificates` where the reference holds `openssl@3`). Use the set `brew fetch --deps` reports today. Whether QEMU and GRUB assembled this way boot an image was not tried.

### ccache

The compiler cache is optional, and is the one piece the tree fetches for you:

```bash
python3 scripts/fetch_ccache.py
```

It downloads upstream's macOS release, checks it against the SHA-256 pinned in the script, and unpacks it into `prebuilt/ccache/`. It does nothing when the pinned release is already there, and changes nothing until the download matches the hash. `tools/set.py` puts it in front of every compile when `prebuilt/ccache/ccache` exists.

### The version stamps

The build reads `prebuilt/clang/llvm-23/.versions/clang.cipd_version` and `prebuilt/gcc/mac-arm64/.versions/gcc.cipd_version` when it is generated, whether or not it uses the tool: `build/config/compiler/gcc.gni` reads the second even though Clang is the default compiler and no GCC is run. Each is a small JSON file with an `instance_id` string. The string does not name anything the build looks up; it is a key, and the build rebuilds what depends on the toolchain when it changes. So for GCC, which this guide does not use, write the stamp and nothing else:

```json
{
  "instance_id": "gcc-15.2.1-elf"
}
```

## Python

The scripts use the Python 3 of the host (3.9.6 on the reference machine) and, for most of them, nothing outside its standard library. Two things need more: `scripts/activate.py` needs PyYAML, and the library scaffolding of `tools/templates/` needs `copier`. `scripts/requirements.txt` lists both:

```bash
python3 -m pip install -r scripts/requirements.txt
```

`scripts/activate.py` downloads the files that a `prebuilt.yml` lists (each with a `name`, a `url` and an optional `sha256`) into `prebuilt/`, and skips a file that is already there with the right hash. The tree does not ship a `prebuilt.yml`. You may write one for the pieces above, but the build does not need one.

## Check what you have

These commands only print versions. Run them from the root of the checkout:

```bash
prebuilt/gn/gn --version
prebuilt/ninja/ninja --version
prebuilt/clang/llvm-23/bin/clang --version
DYLD_FALLBACK_LIBRARY_PATH=$PWD/prebuilt/qemu/lib:$PWD/prebuilt/grub/lib prebuilt/qemu/bin/qemu-system-x86_64 --version
```

On the reference machine they print `2530 (e0a6ab04a113)`, `1.13.2`, a first line beginning `clang version 23.1.0-rc3`, and `QEMU emulator version 11.1.1`. Then confirm the three files that are not programs:

```bash
ls prebuilt/clang/llvm-23/include/c++/v1 | head -1
ls prebuilt/clang/llvm-23/lib/clang/23/lib/x86_64-unknown-none-elf/libclang_rt.builtins.a
ls prebuilt/grub/bin/x86_64-elf-grub-mkimage
```

When all of those answer, run the build and boot as [README.md](README.md) describes.

## What this chapter could not check

- The two download commands for `gn` and `ninja` and the unpacking of the macOS LLVM archive were not run while this chapter was written. For QEMU and GRUB, only the download and extraction of the `x86_64-elf-grub` bottle was run; nothing was booted from a `prebuilt/` assembled by these instructions. The tools in the table were checked by reading the files of the reference machine and printing their versions.
- The names of the two LLVM archives come from the release page, read while this chapter was written (`https://github.com/llvm/llvm-project/releases/expanded_assets/llvmorg-23.1.0-rc3`); their contents were not listed.
- That `gn gen` stops when the GCC stamp is missing is read from `build/config/compiler/gcc.gni`, which reads it without a condition; it was not tried.
- Whether the build works on a host that is not an Apple-silicon Mac, or with newer versions of the tools than the table's, is not known.
- Whether the `prebuilt/clang/llvm-23` of the reference machine is the unmodified macOS archive plus the one runtime file above is not recorded. Its `README.md` says only that the repository's compiler directory held headers and binaries and no runtime.
- Nothing in the tree produces a `prebuilt/` from nothing. A script that did would replace this chapter.

## Where to look next

Go back to [README.md](README.md), build and boot the dev image, and then read [01-concepts.md](01-concepts.md).

## Sources

- `build/config/compiler/clang.gni`, `build/config/compiler/gcc.gni`, `build/config/compiler/rust.gni`: which directories are read, and the version stamps.
- `tools/set.py`: how the compiler directory is chosen.
- `scripts/qemu_boot.py`: what is read from `prebuilt/qemu` and `prebuilt/grub`, and why the loader path is set.
- `scripts/fetch_ccache.py`, `scripts/activate.py`, `scripts/requirements.txt`: the downloads the tree provides.
- `prebuilt/clang/llvm-23/lib/clang/23/lib/x86_64-unknown-none-elf/README.md`, `prebuilt/qemu/.brew/`, `prebuilt/grub/.brew/`, and the `--version` of each tool: what the reference machine holds.
- `docker/dev.dockerfile`: what the development container installs.

Checked against commit `561571b7` on 2026-10-30.
