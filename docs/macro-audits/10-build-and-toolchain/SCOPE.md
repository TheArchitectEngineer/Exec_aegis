# Audit 10: Build & Toolchain

## Priority: LOW (but persistent friction)

Build issues cause wasted debugging time (the stale binary catastrophe).
Header dependency tracking is missing. ARM64 build is broken.

## Files to review

| File | Focus |
|------|-------|
| `Makefile` | Main build system — dependency tracking, clean targets |
| `kernel/arch/arm64/Makefile` | ARM64 build — missing .o files |
| `tools/` | Build helpers, QEMU wrappers, test scripts |
| `user/*/Makefile` | User binary build — clean targets |
| `tests/` | Test harness — boot oracle, integration tests |

## Checklist

### Dependency tracking
- [ ] `-MMD` / `.d` files for automatic header dependency
- [ ] Changing a `.h` file triggers recompilation of dependent `.c`
- [ ] `make clean` removes ALL artifacts (including user binaries?)

### Build reproducibility
- [ ] Same source → same binary (deterministic build)?
- [ ] Timestamp-dependent behavior identified and documented
- [ ] Cross-compilation flags consistent across all Makefiles

### ARM64
- [ ] xhci.c compiles on GCC 15 (array-bounds fix)
- [ ] 8 missing .o files added to ARM64 Makefile
- [ ] smp_percpu_init_bsp() called in ARM64 boot path

### Test infrastructure
- [ ] Boot oracle handles PMM line variation (regex vs exact match)
- [ ] Test timeouts appropriate (BOOT_TIMEOUT=900 vs actual 10-20s)
- [ ] Flaky tests identified and root-caused

## Output format

Same as Audit 1.
