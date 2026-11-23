#include "xenomem.h"

#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/task_info.h>

/*
 * ZERO PRNG GUARANTEE:
 * Notice <stdlib.h> is NOT even included. There is NO rand(), NO random(),
 * NO arc4random(), NO drand48(), NO xorshift, and NO /dev/urandom anywhere.
 * All stochasticity is harvested live from unsynchronized physical data races
 * between Apple Silicon's 4.6 GHz Super-cluster PLL and 3.0 GHz Efficiency-cluster PLL.
 */

#define NUM_SUPER_OSCILLATORS 3
#define NUM_EFF_OSCILLATORS   3
#define MAX_TRACKED_PAGES     256

typedef struct {
    uintptr_t page_base;
    int       materialized;
} TrackedPage;

static uint8_t     *g_vacuum_base = NULL;
static size_t       g_vacuum_size = 0;
static size_t       g_page_size   = 16384;

// Deliberately unsynchronized shared cache lines hammered across heterogeneous clock domains
static volatile uint64_t g_race_soup[8];
static _Atomic int       g_running = 0;

static pthread_t g_super_threads[NUM_SUPER_OSCILLATORS];
static pthread_t g_eff_threads[NUM_EFF_OSCILLATORS];
static pthread_t g_annealer_thread;

// Problem Hamiltonian in the Vacuum
static uint16_t      g_num_vars = 0;
static size_t        g_var_offset[XENO_MAX_VARS];
static int8_t        g_spin_state[XENO_MAX_VARS];     // Current collapsed or uncollapsed spin {-1, +1}
static int8_t        g_var_observed[XENO_MAX_VARS];   // 1 if locked by observation
static int8_t        g_var_pinned[XENO_MAX_VARS];     // 0 = free, +1/-1 = externally measured

static uint32_t      g_num_clauses = 0;
static XenoClause3   g_clauses[XENO_MAX_CLAUSES];

static uint32_t      g_num_couplings = 0;
static XenoCoupling2 g_couplings[XENO_MAX_CLAUSES];

static TrackedPage   g_pages[MAX_TRACKED_PAGES];
static uint32_t      g_num_pages = 0;

// Signal-safe lock-free handoff between MMU SIGBUS trap and background Silicon Annealer
static _Atomic uintptr_t g_fault_page_req = 0;
static _Atomic int       g_fault_ready    = 0;
static uint8_t           g_staged_page[65536]; // Supports up to 64KB page size

// Telemetry
static _Atomic uint64_t g_stat_collapses     = 0;
static _Atomic uint64_t g_stat_decays        = 0;
static _Atomic uint64_t g_stat_total_flips   = 0;
static double           g_stat_total_us      = 0.0;
static uint32_t         g_stat_last_unsat    = 0;
static double           g_stat_shannon       = 0.0;
static double           g_stat_ones_pct      = 50.0;

// -----------------------------------------------------------------------------
// Physical Silicon Clock-Domain Race Oscillators (Deliberate Hardware Collisions)
// -----------------------------------------------------------------------------

static inline uint64_t read_arm_cntvct(void) {
    uint64_t v = 0;
#if defined(__aarch64__)
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
#endif
    return v;
}

static void *super_cluster_oscillator(void *arg) {
    uintptr_t lane = (uintptr_t)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint64_t acc = 0x1000000000000001ULL + lane * 17ULL;
    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        // Deliberate unsynchronized read-modify-write across shared cache lines
        uint64_t t = read_arm_cntvct();
        g_race_soup[lane & 7] ^= (acc + t);
        g_race_soup[(lane + 3) & 7] += (acc ^ (t >> 1));
        acc += 1ULL;
    }
    return NULL;
}

static void *efficiency_cluster_oscillator(void *arg) {
    uintptr_t lane = (uintptr_t)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    uint64_t acc = 0x5555555555555555ULL + lane * 31ULL;
    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        uint64_t t = read_arm_cntvct();
        g_race_soup[lane & 7] += (acc ^ t);
        g_race_soup[(lane + 5) & 7] ^= (acc + (t << 2));
        acc += 3ULL;
    }
    return NULL;
}

// Harvests 1 debiased physical collision byte directly from Super <-> Efficiency race interference
static inline uint8_t harvest_silicon_race_byte(void) {
    uint8_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
        uint64_t a = g_race_soup[bit] ^ g_race_soup[(bit + 3) & 7];
        for (volatile int spin = 0; spin < 5; spin++) {}
        uint64_t b = g_race_soup[(bit + 1) & 7] + g_race_soup[(bit + 5) & 7];
        uint64_t t = read_arm_cntvct();
        uint64_t mix = a ^ (b >> 2) ^ (b >> 9) ^ t;
        int parity = (int)((mix ^ (mix >> 1) ^ (mix >> 4) ^ (mix >> 11)) & 1ULL);
        out = (uint8_t)((out << 1) | parity);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Energy & Silicon-Bath WalkSAT / Ising Thermodynamic Ground-State Crystallizer
// -----------------------------------------------------------------------------

static inline int clause_satisfied(const XenoClause3 *c, const int8_t *s) {
    for (int k = 0; k < 3; k++) {
        int16_t lit = c->lit[k];
        if (lit > 0 && s[lit - 1] > 0) return 1;
        if (lit < 0 && s[(-lit) - 1] < 0) return 1;
    }
    return 0;
}

static uint32_t count_total_violations(const int8_t *s, uint16_t *unsat_clause_indices, uint32_t *out_num_unsat_clauses) {
    uint32_t unsat_c = 0;
    for (uint32_t i = 0; i < g_num_clauses; i++) {
        if (!clause_satisfied(&g_clauses[i], s)) {
            if (unsat_clause_indices) unsat_clause_indices[unsat_c] = (uint16_t)i;
            unsat_c++;
        }
    }
    if (out_num_unsat_clauses) *out_num_unsat_clauses = unsat_c;

    uint32_t unsat_j = 0;
    for (uint32_t i = 0; i < g_num_couplings; i++) {
        const XenoCoupling2 *cp = &g_couplings[i];
        // J > 0 wants s_i == s_j (s_i * s_j > 0); J < 0 wants s_i == -s_j (s_i * s_j < 0)
        int prod = (int)s[cp->var_i] * (int)s[cp->var_j];
        if ((cp->weight_j > 0 && prod < 0) || (cp->weight_j < 0 && prod > 0)) {
            unsat_j += (uint32_t)(cp->weight_j > 0 ? cp->weight_j : -cp->weight_j);
        }
    }
    return unsat_c + unsat_j;
}

static uint32_t delta_violation_if_flipped(int8_t *s, uint16_t v) {
    s[v] = (int8_t)(-s[v]);
    uint32_t after = count_total_violations(s, NULL, NULL);
    s[v] = (int8_t)(-s[v]);
    return after;
}

static void crystallize_vacuum_ground_state(void) {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    // Initialize free (unobserved & unpinned) spins directly from Silicon Race Bath!
    for (uint16_t v = 0; v < g_num_vars; v++) {
        if (g_var_pinned[v] != 0) {
            g_spin_state[v] = g_var_pinned[v];
        } else if (!g_var_observed[v]) {
            g_spin_state[v] = (harvest_silicon_race_byte() & 1) ? +1 : -1;
        }
    }

    uint16_t unsat_list[XENO_MAX_CLAUSES];
    uint32_t unsat_clauses = 0;
    uint64_t flips = 0;
    const uint32_t max_steps = 60000;

    for (uint32_t step = 0; step < max_steps; step++) {
        uint32_t total_err = count_total_violations(g_spin_state, unsat_list, &unsat_clauses);
        if (total_err == 0) {
            g_stat_last_unsat = 0;
            break;
        }
        g_stat_last_unsat = total_err;

        // Periodic high-temperature Kramers Silicon-Bath Quench if trapped in deep frustration
        if (step > 0 && (step % 4000) == 0) {
            for (uint16_t v = 0; v < g_num_vars; v++) {
                if (!g_var_observed[v] && g_var_pinned[v] == 0) {
                    g_spin_state[v] = (harvest_silicon_race_byte() & 1) ? +1 : -1;
                }
            }
            continue;
        }

        uint8_t thermal_byte = harvest_silicon_race_byte();

        if (unsat_clauses > 0) {
            // Pick a frustrated clause using pure silicon race byte
            uint16_t c_idx = unsat_list[((uint32_t)thermal_byte * 257U + (uint32_t)harvest_silicon_race_byte()) % unsat_clauses];
            const XenoClause3 *cl = &g_clauses[c_idx];

            // ~35% Silicon Thermal Noise Tunneling (90 / 256) vs ~65% Greedy Energy Descent
            if (thermal_byte < 90) {
                int slot = harvest_silicon_race_byte() % 3;
                int16_t lit = cl->lit[slot];
                uint16_t v = (uint16_t)((lit > 0 ? lit : -lit) - 1);
                if (!g_var_observed[v] && g_var_pinned[v] == 0) {
                    g_spin_state[v] = (int8_t)(-g_spin_state[v]);
                    flips++;
                }
            } else {
                uint16_t best_v = UINT16_MAX;
                uint32_t best_e = UINT32_MAX;
                for (int slot = 0; slot < 3; slot++) {
                    int16_t lit = cl->lit[slot];
                    uint16_t v = (uint16_t)((lit > 0 ? lit : -lit) - 1);
                    if (g_var_observed[v] || g_var_pinned[v] != 0) continue;
                    uint32_t e_after = delta_violation_if_flipped(g_spin_state, v);
                    if (e_after < best_e) {
                        best_e = e_after;
                        best_v = v;
                    }
                }
                if (best_v != UINT16_MAX) {
                    g_spin_state[best_v] = (int8_t)(-g_spin_state[best_v]);
                    flips++;
                }
            }
        } else {
            // Frustrated pairwise Ising entanglement couplings only
            for (uint32_t i = 0; i < g_num_couplings; i++) {
                const XenoCoupling2 *cp = &g_couplings[i];
                int prod = (int)g_spin_state[cp->var_i] * (int)g_spin_state[cp->var_j];
                if ((cp->weight_j > 0 && prod < 0) || (cp->weight_j < 0 && prod > 0)) {
                    uint16_t cand = (thermal_byte & 1) ? cp->var_i : cp->var_j;
                    if (g_var_observed[cand] || g_var_pinned[cand] != 0) {
                        cand = (cand == cp->var_i) ? cp->var_j : cp->var_i;
                    }
                    if (!g_var_observed[cand] && g_var_pinned[cand] == 0) {
                        g_spin_state[cand] = (int8_t)(-g_spin_state[cand]);
                        flips++;
                        break;
                    }
                }
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    g_stat_total_us += (double)(ts1.tv_sec - ts0.tv_sec) * 1e6 + (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e3;
    atomic_fetch_add_explicit(&g_stat_total_flips, flips, memory_order_relaxed);
}

static void *annealer_coordinator_loop(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        uintptr_t req_page = atomic_load_explicit(&g_fault_page_req, memory_order_acquire);
        if (req_page != 0) {
            // 1. Solve joint Hamiltonian consistent with all already-observed & pinned spins!
            crystallize_vacuum_ground_state();

            // 2. Materialize ONLY the 16 KB page that the observer just dereferenced!
            memset(g_staged_page, 0, g_page_size);
            size_t page_offset_in_vac = (size_t)(req_page - (uintptr_t)g_vacuum_base);
            for (uint16_t v = 0; v < g_num_vars; v++) {
                size_t off = g_var_offset[v];
                if (off >= page_offset_in_vac && off < page_offset_in_vac + g_page_size) {
                    g_staged_page[off - page_offset_in_vac] = (uint8_t)g_spin_state[v];
                    // Observation locks this variable's reality until Vacuum Decay!
                    g_var_observed[v] = 1;
                }
            }

            // Record materialized page for potential future retrocausal Vacuum Decay
            int found = 0;
            for (uint32_t p = 0; p < g_num_pages; p++) {
                if (g_pages[p].page_base == req_page) {
                    g_pages[p].materialized = 1;
                    found = 1;
                    break;
                }
            }
            if (!found && g_num_pages < MAX_TRACKED_PAGES) {
                g_pages[g_num_pages].page_base = req_page;
                g_pages[g_num_pages].materialized = 1;
                g_num_pages++;
            }

            atomic_store_explicit(&g_fault_page_req, 0, memory_order_release);
            atomic_store_explicit(&g_fault_ready, 1, memory_order_release);
        } else {
#if defined(__aarch64__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Async-Signal-Safe ARM64 MMU Wavefunction-Collapse Trap Handler
// -----------------------------------------------------------------------------

static void xeno_mmu_observer_trap(int sig, siginfo_t *info, void *uap) {
    (void)sig;
    (void)uap;
    uintptr_t addr = (uintptr_t)info->si_addr;
    uintptr_t vac_start = (uintptr_t)g_vacuum_base;
    if (!g_vacuum_base || addr < vac_start || addr >= vac_start + g_vacuum_size) {
        // Real bug outside the XenoMem vacuum -> restore default crash behavior
        signal(sig, SIG_DFL);
        return;
    }

    uintptr_t page_base = addr & ~(uintptr_t)(g_page_size - 1);

    // Hand faulting page address to persistent Silicon Race Annealer
    atomic_store_explicit(&g_fault_ready, 0, memory_order_release);
    atomic_store_explicit(&g_fault_page_req, page_base, memory_order_release);

    while (atomic_load_explicit(&g_fault_ready, memory_order_acquire) == 0) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    // Materialize collapsed wavefunction onto the faulting Mach VM page and lock read-only!
    if (mprotect((void *)page_base, g_page_size, PROT_READ | PROT_WRITE) != 0) {
        _exit(99);
    }
    memcpy((void *)page_base, g_staged_page, g_page_size);
    if (mprotect((void *)page_base, g_page_size, PROT_READ) != 0) {
        _exit(98);
    }

    atomic_fetch_add_explicit(&g_stat_collapses, 1, memory_order_relaxed);
    // Returning re-executes the exact faulting ARM64 load instruction seamlessly!
}

// -----------------------------------------------------------------------------
// Public XenoMem API
// -----------------------------------------------------------------------------

int xeno_init(size_t virtual_universe_bytes, int8_t **out_vacuum_ptr) {
    g_page_size = (size_t)getpagesize();
    g_vacuum_size = ((virtual_universe_bytes + g_page_size - 1) / g_page_size) * g_page_size;

    void *ptr = mmap(NULL, g_vacuum_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ptr == MAP_FAILED) return -1;
    g_vacuum_base = (uint8_t *)ptr;

    xeno_clear_hamiltonian();
    g_num_pages = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = xeno_mmu_observer_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, NULL) != 0) return -2;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) return -3;

    atomic_store_explicit(&g_running, 1, memory_order_release);
    for (uintptr_t i = 0; i < NUM_SUPER_OSCILLATORS; i++) {
        pthread_create(&g_super_threads[i], NULL, super_cluster_oscillator, (void *)i);
    }
    for (uintptr_t i = 0; i < NUM_EFF_OSCILLATORS; i++) {
        pthread_create(&g_eff_threads[i], NULL, efficiency_cluster_oscillator, (void *)i);
    }
    pthread_create(&g_annealer_thread, NULL, annealer_coordinator_loop, NULL);

    // Let cross-cluster race soup reach chaotic steady-state (~2 ms)
    usleep(2000);
    xeno_measure_silicon_bath_entropy(&g_stat_shannon, &g_stat_ones_pct);

    *out_vacuum_ptr = (int8_t *)g_vacuum_base;
    return 0;
}

void xeno_shutdown(void) {
    if (!g_vacuum_base) return;
    atomic_store_explicit(&g_running, 0, memory_order_release);
    for (int i = 0; i < NUM_SUPER_OSCILLATORS; i++) pthread_join(g_super_threads[i], NULL);
    for (int i = 0; i < NUM_EFF_OSCILLATORS; i++) pthread_join(g_eff_threads[i], NULL);
    pthread_join(g_annealer_thread, NULL);

    signal(SIGBUS, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    munmap(g_vacuum_base, g_vacuum_size);
    g_vacuum_base = NULL;
}

void xeno_clear_hamiltonian(void) {
    // Dissolve any previously materialized pages back into PROT_NONE void
    for (uint32_t p = 0; p < g_num_pages; p++) {
        if (g_pages[p].materialized) {
            mprotect((void *)g_pages[p].page_base, g_page_size, PROT_NONE);
            g_pages[p].materialized = 0;
        }
    }
    g_num_pages = 0;
    g_num_vars = 0;
    g_num_clauses = 0;
    g_num_couplings = 0;
    atomic_store_explicit(&g_stat_total_flips, 0, memory_order_relaxed);
    g_stat_total_us = 0.0;
    memset(g_var_observed, 0, sizeof(g_var_observed));
    memset(g_var_pinned, 0, sizeof(g_var_pinned));
    memset(g_spin_state, 0, sizeof(g_spin_state));
}

int xeno_bind_variable(uint16_t var_id, size_t byte_offset_in_vacuum) {
    if (var_id >= XENO_MAX_VARS || byte_offset_in_vacuum >= g_vacuum_size) return -1;
    g_var_offset[var_id] = byte_offset_in_vacuum;
    if (var_id + 1 > g_num_vars) g_num_vars = var_id + 1;
    return 0;
}

int xeno_add_3sat_clause(int16_t lit0, int16_t lit1, int16_t lit2) {
    if (g_num_clauses >= XENO_MAX_CLAUSES) return -1;
    g_clauses[g_num_clauses].lit[0] = lit0;
    g_clauses[g_num_clauses].lit[1] = lit1;
    g_clauses[g_num_clauses].lit[2] = lit2;
    g_num_clauses++;
    return 0;
}

int xeno_add_ising_coupling(uint16_t var_i, uint16_t var_j, int16_t j_coupling) {
    if (g_num_couplings >= XENO_MAX_CLAUSES) return -1;
    g_couplings[g_num_couplings].var_i = var_i;
    g_couplings[g_num_couplings].var_j = var_j;
    g_couplings[g_num_couplings].weight_j = j_coupling;
    g_num_couplings++;
    return 0;
}

int xeno_impose_measurement(uint16_t var_id, int8_t pinned_spin) {
    if (var_id >= g_num_vars) return -1;
    g_var_pinned[var_id] = (pinned_spin >= 0) ? +1 : -1;
    g_spin_state[var_id] = g_var_pinned[var_id];

    // Check if the new observation violates the Hamiltonian given previously observed pages!
    uint32_t violations = count_total_violations(g_spin_state, NULL, NULL);
    int was_observed = g_var_observed[var_id];

    if (violations > 0 || was_observed) {
        // RETROCAUSAL VACUUM DECAY!
        // Dissolve all materialized pages back into uncollapsed PROT_NONE quantum foam
        // so the next pointer dereference anywhere re-anneals a consistent timeline!
        for (uint32_t p = 0; p < g_num_pages; p++) {
            if (g_pages[p].materialized) {
                mprotect((void *)g_pages[p].page_base, g_page_size, PROT_NONE);
                g_pages[p].materialized = 0;
                atomic_fetch_add_explicit(&g_stat_decays, 1, memory_order_relaxed);
            }
        }
        memset(g_var_observed, 0, sizeof(g_var_observed));
    }
    return 0;
}

void xeno_measure_silicon_bath_entropy(double *out_shannon_bits, double *out_ones_pct) {
    uint32_t hist[256] = {0};
    uint32_t ones = 0;
    const int samples = 8192;
    for (int i = 0; i < samples; i++) {
        uint8_t b = harvest_silicon_race_byte();
        hist[b]++;
        for (int k = 0; k < 8; k++) ones += (b >> k) & 1U;
    }
    double h = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            double p = (double)hist[i] / (double)samples;
            h -= p * log2(p);
        }
    }
    if (out_shannon_bits) *out_shannon_bits = h;
    if (out_ones_pct) *out_ones_pct = 100.0 * (double)ones / (double)(samples * 8);
}

XenoTelemetry xeno_get_telemetry(void) {
    XenoTelemetry t;
    t.shannon_entropy_bits_per_byte = g_stat_shannon;
    t.ones_ratio_percent = g_stat_ones_pct;
    t.total_page_collapses = atomic_load_explicit(&g_stat_collapses, memory_order_relaxed);
    t.total_vacuum_decays = atomic_load_explicit(&g_stat_decays, memory_order_relaxed);
    t.total_anneal_race_flips = atomic_load_explicit(&g_stat_total_flips, memory_order_relaxed);
    t.total_collapse_latency_us = g_stat_total_us;
    t.last_unsatisfied_clauses = g_stat_last_unsat;

    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info, &count) == KERN_SUCCESS) {
        t.phys_footprint_bytes = (uint64_t)vm_info.phys_footprint;
    } else {
        t.phys_footprint_bytes = 0;
    }
    return t;
}
