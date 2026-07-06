# Dynamic Linker (ld.so) Roadmap

This document tracks the progress of implementing a dynamic linker for the custom AArch64 OS.

## Phase 1: Foundation & PIE Loading (Current)
- [x] Basic PIE entry point (`_start`) in assembly.
- [x] Self-relocation logic for `ld.so`.
- [x] Minimal Syscall wrappers (mmap, open, read, write).
- [x] Simple ELF loader (header parsing, segment mapping).
- [x] Basic `R_AARCH64_RELATIVE` relocation support.
- [x] First successful jump to a statically linked PIE app.

## Phase 2: Dynamic Dependencies & Symbol Resolution
- [ ] Parse `.dynamic` section of the main executable.
- [ ] Identify and load `DT_NEEDED` dependencies (e.g., `libc.so`).
- [ ] Implement Symbol Hash Table lookup (`DT_HASH` or `DT_GNU_HASH`).
- [ ] Implement global symbol resolution across multiple objects.
- [ ] Support `R_AARCH64_GLOB_DAT` (Global Data) relocations.
- [ ] Support `R_AARCH64_JUMP_SLOT` (PLT) relocations.

## Phase 3: Advanced Features
- [ ] Lazy Binding (PLT stub handling).
- [ ] Thread Local Storage (TLS) support.
- [ ] Initialization/Finalization routines (`DT_INIT`/`DT_FINI`).
- [ ] Environment variables and `auxv` passing from kernel.
- [ ] Search paths (RPATH/RUNPATH).

## Phase 4: Integration
- [ ] Replace kernel-side static loader with `ld.so` for all user apps.
- [ ] Compile all apps as dynamic PIEs.
- [ ] Standardize `libc.so` as a shared object.
