<div align="center">

# xenomem

### Demand-paged constraint memory on Apple Silicon: reserve 64 GB of `PROT_NONE` address space, attach 3-SAT / Ising rules, and trigger WalkSAT solves via `SIGBUS` reads & writes.

[![CI](https://github.com/rossbuckley1990-hash/xenomem/actions/workflows/ci.yml/badge.svg)](https://github.com/rossbuckley1990-hash/xenomem/actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/pure_C11-zero_dependencies-black)
![PRNGs](https://img.shields.io/badge/PRNG_imports-0_(verified_in_CI)-brightgreen)
![Apple Silicon](https://img.shields.io/badge/macOS-ARM64-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)

</div>

---

## What It Actually Is (No Hype)

**XenoMem** is a ~600-line C11 virtual-memory experiment for macOS / Apple Silicon that hides an incremental WalkSAT / Ising solver behind hardware MMU page faults (`SIGBUS` / `SIGSEGV`) and ARM64 exception-syndrome (`ESR_EL1`) write traps:

```c
// 1. Reserve 64 GB of unmapped PROT_NONE virtual address space (~1.5 MB process footprint)
volatile int8_t *reality;
xeno_init(64ULL * GB, (int8_t **)&reality);

// 2. Bind spin variables (+/-1) to arbitrary offsets across the 64 GB region & add couplings
xeno_bind_variable(0, 0ULL * GB + 256);
xeno_bind_variable(1, 48ULL * GB + 256);
xeno_add_ising_coupling(0, 1, -5);           // Anti-ferromagnetic constraint: s_0 == -s_1

// 3. Reading an unmapped pointer faults into SIGBUS, runs WalkSAT on a worker thread,
//    and maps ONLY that 16 KB page:
int8_t a = reality[0ULL * GB + 256];         // -> +1
int8_t b = reality[48ULL * GB + 256];        // -> -1

// 4. Writing directly to a bound pointer traps via ARM64 ESR_EL1 (WnR=1), pins the variable,
//    and resets contradicted pages back to PROT_NONE so subsequent reads re-solve consistently:
reality[48ULL * GB + 256] = +1;
int8_t a_new = reality[0ULL * GB + 256];     // -> -1
```

---

## How Each Mechanism Works Under the Hood

1. **Lazy Demand-Paged Materialization (`mmap` + `SIGBUS`)**
   `xeno_init` reserves a 64 GB `MAP_PRIVATE | MAP_ANON` region with `PROT_NONE`. When user code reads an unmapped page, `xeno_mmu_observer_trap` sends the faulting address over an async-signal-safe POSIX pipe to a parked coordinator thread, stages the 16 KB page containing any bound variables on that page, maps it `PROT_READ`, and resumes the faulting load instruction.
2. **Direct ARM64 Pointer-Write Interception (`ESR_EL1.WnR`)**
   When user code writes to `reality[offset] = +1`, the handler inspects the ARM64 Exception Syndrome Register (`uc_mcontext->__es.__esr`, bit 6 `WnR`), decodes the scalar `STRB`/`STURB` source register, pins that variable's spin, and invalidates stale pages. Writes to unbound offsets map `PROT_READ | PROT_WRITE` cleanly without hanging.
3. **Single-Syscall Cache Invalidation ("Vacuum Decay")**
   Pinning a variable that contradicts previously materialized pages (or calling `xeno_clear_hamiltonian`) re-maps the entire virtual range back to `PROT_NONE` via a single `mmap(..., MAP_FIXED | PROT_NONE)` call — supporting millions of pages with no fixed page-table cap and releasing physical pages back to XNU.
4. **Demand-Gated Cross-QoS Contention Jitter (`0.0%` Idle CPU)**
   `<stdlib.h>` is not included and CI verifies `nm -u` imports zero `rand`, `random`, `arc4random`, `getentropy`, or `open` symbols. During an active solve, two `QOS_CLASS_USER_INTERACTIVE` threads and two `QOS_CLASS_BACKGROUND` threads wake on a condition variable and race unsynchronized read-modify-writes with `cntvct_el0` across shared cache lines to drive stochastic WalkSAT steps, then park immediately back at `0.0%` CPU when the page fault completes.
   * **Measuring unpredictability honestly**: A plain byte histogram (`Shannon entropy`) scores `8.00` even on a deterministic `0..255` counter. `demo.c` therefore reports **NIST SP 800-90B Min-Entropy** ($H_\infty$), **First-Order Markov Conditional Entropy** $H(X_t \mid X_{t-1})$ (`0.00` for a counter), **Lag-1 Serial Correlation**, and an explicit **Oscillators-OFF vs. Oscillators-ON** control.
5. **$O(1)$ Occurrence-Indexed WalkSAT with Backtracking**
   Variables maintain occurrence lists into clauses (`XENO_MAX_VARS = 1024`, `XENO_MAX_CLAUSES = 8192`) so flip deltas evaluate in $O(\text{degree})$ time. Unsatisfied partial assignments are never locked as observed; if earlier partial page observations trap a later solve, `xenomem` automatically invalidates prior pages (preserving explicit pins) and re-solves globally.

---

## Build & Run

```bash
git clone https://github.com/rossbuckley1990-hash/xenomem.git
cd xenomem && make && ./xenomem
```

Verify zero PRNG / entropy-device symbol imports:
```bash
nm -u ./xenomem
```

---

## License

MIT © Ross Buckley
