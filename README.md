<div align="center">

# xenomem

### What if reading an unallocated pointer didn't crash your program—<br>it solved an NP-hard problem using CPU data races?

[![CI](https://github.com/rossbuckley1990-hash/xenomem/actions/workflows/ci.yml/badge.svg)](https://github.com/rossbuckley1990-hash/xenomem/actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/pure_C11-zero_dependencies-black)
![PRNGs](https://img.shields.io/badge/rand()_calls-0_(verified_in_CI)-brightgreen)
![Apple Silicon](https://img.shields.io/badge/Apple_Silicon-M1–M5-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)

**XenoMem** breaks every rule you learned in CS 101.<br>
Declare a **64 GB** array backed by **0 bytes of RAM**, attach logical rules to it, **never call a solver**, and just read `reality[index]` straight out of empty memory.

</div>

---

## Code That Should Immediately Segfault (But Doesn't)

```c
// 1. Create 64 Gigabytes of completely unallocated, unreadable void (0 KB of RAM)
volatile int8_t *reality;
xeno_init(64ULL * GB, (int8_t **)&reality);

// 2. Attach rules (e.g. 425 boolean 3-SAT clauses, or entangle two bytes 48 GB apart)
xeno_bind_variable(0, 0ULL * GB + 256);    // Byte at 0 GB
xeno_bind_variable(1, 48ULL * GB + 256);   // Byte at 48 GB (in unallocated void!)
xeno_add_ising_coupling(0, 1, -5);         // Law: Byte[0 GB] must always oppose Byte[48 GB]

// 3. DO NOT CALL A SOLVER. Just dereference the unallocated pointers:
int8_t a = reality[0ULL * GB + 256];       // -> +1  (collapsed on read!)
int8_t b = reality[48ULL * GB + 256];      // -> -1  (already entangled 48 GB away!)
```

Now watch what happens if you create a **time-travel paradox**:

```c
// Force Byte[48 GB] to be +1 — contradicting the +1 you ALREADY read at Byte[0 GB]!
xeno_impose_measurement(1, +1);

// Nobody writes to Byte[0 GB]. Just read the exact same pointer again:
int8_t a_new = reality[0ULL * GB + 256];   // -> -1  (the past rewrote itself!)
```

---

## Wait... How Is This Possible? (The 20-Second Explanation)

Every computer program since 1945 treats **segfaults** and **multi-core data races** as fatal bugs. XenoMem uses them as its CPU:

1. **64 GB of "Schrödinger Memory" (`0 KB` RAM):** `xeno_init` reserves 64 GB of virtual address space marked `PROT_NONE`. Nothing exists in physical RAM until you look at it.
2. **Segfaults = Wavefunction Collapse:** The nanosecond your CPU executes a load instruction (`ldr`) on an unobserved address, the hardware MMU freezes the instruction mid-flight (`SIGBUS`). XenoMem solves only what is needed, materializes that single `16 KB` page (`mprotect`), and resumes your exact CPU instruction as if the answer was always there.
3. **CPU Data Races = True Physical Annealing (`0` `rand()` calls):** `<stdlib.h>` isn't even `#include`d in `xenomem.c` (our GitHub Actions CI fails if `nm -u` finds `rand`, `arc4random`, or `open`). Instead, 3 threads on Apple Silicon's **4.6 GHz Performance clock** collide head-on *without locks* against 3 threads on the **3.0 GHz Efficiency clock**. Real physical silicon clock-drift and cache collisions produce **`7.98 / 8.00 bits/byte`** of true thermal entropy to tunnel out of local minima.
4. **"Vacuum Decay" (Self-Healing Past):** When a new observation 48 GB away contradicts a page you already read, XenoMem dissolves the old page back into `PROT_NONE` void. Next time your code reads that pointer, reality re-crystallizes into a consistent timeline.

---

## Try It on Your Mac in 5 Seconds

```bash
git clone https://github.com/rossbuckley1990-hash/xenomem.git
cd xenomem && make && ./xenomem
```

### Live Output on an Apple M5 MacBook Air (`1.59 MB` RAM used for a `64.0 GB` universe)

```text
[1] PHYSICAL SILICON CONTENTION BATH (ZERO PRNG / ZERO SYSCALL ENTROPY AUDIT)
    • Architecture     : 3 Super-Core Oscillators (4.6GHz) vs 3 Efficiency-Core Oscillators (3.0GHz)
    • Coupling Medium  : Unsynchronized C Data-Races across M5 Cache-Coherency Fabric
    • Measured Entropy : 7.9810 / 8.0000 bits/byte  |  Ones Balance: 49.76%

[2] OBSERVER-COLLAPSED NP-HARD 3-SAT IN A 64 GB VOID (N=100 Spins, M=425 Clauses, α=4.25)
    • Solver Function Called : NONE (Triggered purely by raw C pointer dereference `reality[addr]`)
    • Clauses Crystallized   : 425 / 425 (100.0% Ground State Satisfaction!)
    • Silicon Race Flips     : 11055 physical collision-driven flips in 41109.0 µs
    • Materialized Pages     : 10 × 16 KB out of 64.0 GB (Total Process Footprint: 1.59 MB)

[3] NON-LOCAL ENTANGLEMENT ACROSS 48 GB VOID & RETROCAUSAL VACUUM DECAY
    • Timeline 1 Observation : reality[0 GB] = +1  |  reality[48 GB] = -1  (Entangled Bell Pair: VERIFIED)
    • Paradox Injected @48GB : Pinning Bob = +1 contradicts Timeline 1 -> Triggered 2 Vacuum Decays!
    • Timeline 2 Re-Read     : reality[0 GB] = -1  |  reality[48 GB] = +1  (Past Self-Healed: VERIFIED!)
```

Verify for yourself that there isn't a single random-number generator or `/dev/urandom` call in the compiled binary:
```bash
nm -u ./xenomem
```

---

## License

MIT © Ross Buckley
