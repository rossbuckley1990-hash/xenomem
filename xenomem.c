#include "xenomem.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/task_info.h>

/*
 * ZERO PRNG GUARANTEE:
 * <stdlib.h> is NOT included. Zero rand(), random(), arc4random(), drand48(),
 * getentropy(), or open("/dev/urandom") symbols exist in this binary (enforced in CI).
 * Stochasticity comes strictly from demand-gated cross-QoS cache-line contention
 * combined with ARM64 virtual counter jitter (cntvct_el0).
 */

#define NUM_SUPER_OSCILLATORS 2
#define NUM_EFF_OSCILLATORS   2
#define MAX_VAR_OCCURRENCES   256
#define RACE_RING_MASK        4095U
#define PAGE_CACHE_SLOTS      8192U
#define PAGE_CACHE_MASK       (PAGE_CACHE_SLOTS - 1U)

typedef struct {
    uintptr_t fault_addr;
    int32_t   is_write;
    int32_t   written_byte;
} TrapRequest;

typedef struct {
    int32_t advance_pc;
    int32_t ok;
} TrapReply;

typedef struct {
    uintptr_t page_base;
    uint64_t  epoch;
} PageEpochEntry;

static uint8_t     *g_vacuum_base = NULL;
static size_t       g_vacuum_size = 0;
static size_t       g_page_size   = 16384;

// Demand-gated oscillator threads (0.0% CPU when parked!)
static pthread_mutex_t   g_osc_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_osc_cond   = PTHREAD_COND_INITIALIZER;
static _Atomic int       g_osc_active = 0;
static _Atomic int       g_running    = 0;

static volatile uint64_t g_race_soup[8];
static volatile uint8_t  g_race_ring[RACE_RING_MASK + 1];
static _Atomic uint32_t  g_race_cursor = 0;

static pthread_t g_super_threads[NUM_SUPER_OSCILLATORS];
static pthread_t g_eff_threads[NUM_EFF_OSCILLATORS];
static pthread_t g_coord_thread;

// Async-signal-safe pipe IPC between SIGBUS handler and coordinator thread
static int         g_req_pipe[2] = {-1, -1};
static int         g_ack_pipe[2] = {-1, -1};
static atomic_flag g_trap_spinlock = ATOMIC_FLAG_INIT;

// Unlimited-page O(1) epoch invalidation tracking
static uint64_t       g_current_epoch = 1;
static uint64_t       g_active_materialized_pages = 0;
static PageEpochEntry g_page_cache[PAGE_CACHE_SLOTS];
static uint8_t        g_staged_page[65536];

// Sparse-capable variable & Hamiltonian tables
static uint16_t      g_max_var_plus_one = 0;
static uint8_t       g_var_bound[XENO_MAX_VARS];
static size_t        g_var_offset[XENO_MAX_VARS];
static int8_t        g_spin_state[XENO_MAX_VARS];
static int8_t        g_var_observed[XENO_MAX_VARS];
static int8_t        g_var_pinned[XENO_MAX_VARS];
static int           g_ground_state_valid = 0;
static int           g_anneal_attempted   = 0;

static uint32_t      g_num_clauses = 0;
static XenoClause3   g_clauses[XENO_MAX_CLAUSES];

static uint16_t      g_pos_occ_len[XENO_MAX_VARS];
static uint16_t      g_neg_occ_len[XENO_MAX_VARS];
static uint16_t      g_pos_occ[XENO_MAX_VARS][MAX_VAR_OCCURRENCES];
static uint16_t      g_neg_occ[XENO_MAX_VARS][MAX_VAR_OCCURRENCES];

static uint8_t       g_clause_true_cnt[XENO_MAX_CLAUSES];
static int32_t       g_unsat_pos[XENO_MAX_CLAUSES];
static uint16_t      g_unsat_list[XENO_MAX_CLAUSES];
static uint32_t      g_num_unsat_clauses = 0;

static uint32_t      g_num_couplings = 0;
static XenoCoupling2 g_couplings[XENO_MAX_CLAUSES];

// Telemetry
static _Atomic uint64_t g_stat_collapses   = 0;
static _Atomic uint64_t g_stat_decays      = 0;
static _Atomic uint64_t g_stat_total_flips = 0;
static double           g_stat_total_us    = 0.0;
static uint32_t         g_stat_last_unsat  = 0;
static double           g_stat_shannon     = 0.0;
static double           g_stat_ones_pct    = 50.0;

// -----------------------------------------------------------------------------
// Demand-Gated Cross-QoS Silicon Contention Oscillators (0% Idle CPU)
// -----------------------------------------------------------------------------

static inline uint64_t read_arm_cntvct(void) {
    uint64_t v = 0;
#if defined(__aarch64__)
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = (uint64_t)ts.tv_nsec;
#endif
    return v;
}

static void set_oscillators_active(int active) {
    pthread_mutex_lock(&g_osc_mutex);
    atomic_store_explicit(&g_osc_active, active, memory_order_release);
    if (active) {
        pthread_cond_broadcast(&g_osc_cond);
    }
    pthread_mutex_unlock(&g_osc_mutex);
}

static void *oscillator_worker(void *arg) {
    uintptr_t encoded = (uintptr_t)arg;
    int is_super = (encoded < 100U);
    uintptr_t lane = is_super ? encoded : (encoded - 100U);

    pthread_set_qos_class_self_np(is_super ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND, 0);

    uint64_t acc = (is_super ? 0x1000000000000001ULL : 0x5555555555555555ULL) + lane * 37ULL;
    uint32_t idx = (uint32_t)(lane * 251U + (is_super ? 0U : 1021U));
    const uint32_t step = is_super ? 7U : 13U;

    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        if (!atomic_load_explicit(&g_osc_active, memory_order_acquire)) {
            pthread_mutex_lock(&g_osc_mutex);
            while (atomic_load_explicit(&g_running, memory_order_relaxed) &&
                   !atomic_load_explicit(&g_osc_active, memory_order_relaxed)) {
                pthread_cond_wait(&g_osc_cond, &g_osc_mutex);
            }
            pthread_mutex_unlock(&g_osc_mutex);
            if (!atomic_load_explicit(&g_running, memory_order_relaxed)) break;
        }

        // Tight unsynchronized race burst before re-checking park flag
        for (int burst = 0; burst < 256; burst++) {
            uint64_t t = read_arm_cntvct();
            if (is_super) {
                uint64_t s0 = (g_race_soup[lane & 7U] ^= (acc + t));
                uint64_t s1 = (g_race_soup[(lane + 3U) & 7U] += (acc ^ (t >> 1)));
                g_race_ring[idx & RACE_RING_MASK] ^= (uint8_t)((s0 ^ (s1 >> 3) ^ t) & 0xFFU);
                acc += 1ULL;
            } else {
                uint64_t e0 = (g_race_soup[lane & 7U] += (acc ^ t));
                uint64_t e1 = (g_race_soup[(lane + 5U) & 7U] ^= (acc + (t << 2)));
                g_race_ring[idx & RACE_RING_MASK] += (uint8_t)((e0 ^ (e1 >> 5) ^ (t >> 1)) & 0xFFU);
                acc += 3ULL;
            }
            idx += step;
        }
    }
    return NULL;
}

static inline uint8_t harvest_race_fast(void) {
    uint32_t cur = atomic_fetch_add_explicit(&g_race_cursor, 1U, memory_order_relaxed);
    uint64_t t = read_arm_cntvct();
    uint8_t r0 = g_race_ring[cur & RACE_RING_MASK];
    uint8_t r1 = g_race_ring[(cur * 17U + (uint32_t)t) & RACE_RING_MASK];
    uint64_t soup = g_race_soup[cur & 7U] ^ g_race_soup[(cur + 3U) & 7U] ^ t;
    return (uint8_t)(r0 ^ r1 ^ (uint8_t)soup ^ (uint8_t)(soup >> 9));
}

static inline uint8_t sample_entropy_byte(int use_race_soup) {
    uint8_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
        for (volatile int d = 0; d < 4; d++) {}
        uint64_t t = read_arm_cntvct();
        int bval;
        if (use_race_soup) {
            uint64_t a = g_race_soup[bit] ^ g_race_soup[(bit + 3) & 7];
            uint64_t b = g_race_soup[(bit + 1) & 7] + g_race_soup[(bit + 5) & 7];
            uint8_t ring = g_race_ring[(uint32_t)(t + (uint64_t)bit * 251ULL) & RACE_RING_MASK];
            uint64_t mix = a ^ (b >> 2) ^ (b >> 9) ^ t ^ ring;
            bval = (int)((mix ^ (mix >> 1) ^ (mix >> 4) ^ (mix >> 11)) & 1ULL);
        } else {
            // Parked A/B baseline: timer counter LSBs only (no cross-thread contention!)
            bval = (int)(t & 1ULL);
        }
        out = (uint8_t)((out << 1) | bval);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Single-Syscall Infinite-Page Vacuum Invalidation
// -----------------------------------------------------------------------------

static void invalidate_all_materialized_pages(int count_as_decay) {
    if (g_active_materialized_pages > 0 && g_vacuum_base != NULL) {
        void *rc = mmap(g_vacuum_base, g_vacuum_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        (void)rc;
        if (count_as_decay) {
            atomic_fetch_add_explicit(&g_stat_decays, g_active_materialized_pages, memory_order_relaxed);
        }
        g_active_materialized_pages = 0;
    }
    g_current_epoch++;
    memset(g_var_observed, 0, sizeof(g_var_observed));
    g_ground_state_valid = 0;
    g_anneal_attempted = 0;
}

static inline int page_is_cached_materialized(uintptr_t page_base) {
    uint32_t slot = (uint32_t)(((page_base >> 14) * 2654435761ULL) & PAGE_CACHE_MASK);
    return (g_page_cache[slot].epoch == g_current_epoch && g_page_cache[slot].page_base == page_base);
}

static inline void mark_page_cached_materialized(uintptr_t page_base) {
    uint32_t slot = (uint32_t)(((page_base >> 14) * 2654435761ULL) & PAGE_CACHE_MASK);
    if (!(g_page_cache[slot].epoch == g_current_epoch && g_page_cache[slot].page_base == page_base)) {
        g_active_materialized_pages++;
    }
    g_page_cache[slot].page_base = page_base;
    g_page_cache[slot].epoch = g_current_epoch;
}

// -----------------------------------------------------------------------------
// O(1) Occurrence-Indexed WalkSAT / Ising Solver
// -----------------------------------------------------------------------------

static inline void unsat_insert(uint16_t c) {
    if (g_unsat_pos[c] >= 0) return;
    g_unsat_pos[c] = (int32_t)g_num_unsat_clauses;
    g_unsat_list[g_num_unsat_clauses++] = c;
}

static inline void unsat_remove(uint16_t c) {
    int32_t pos = g_unsat_pos[c];
    if (pos < 0) return;
    uint32_t last_idx = --g_num_unsat_clauses;
    uint16_t moved_c = g_unsat_list[last_idx];
    g_unsat_list[pos] = moved_c;
    g_unsat_pos[moved_c] = pos;
    g_unsat_pos[c] = -1;
}

static void rebuild_incremental_state(void) {
    g_num_unsat_clauses = 0;
    for (uint32_t c = 0; c < g_num_clauses; c++) {
        g_unsat_pos[c] = -1;
        uint8_t tc = 0;
        for (int k = 0; k < 3; k++) {
            int16_t lit = g_clauses[c].lit[k];
            int dup = 0;
            for (int prev = 0; prev < k; prev++) {
                if (g_clauses[c].lit[prev] == lit) dup = 1;
            }
            if (dup) continue;
            if ((lit > 0 && g_spin_state[lit - 1] > 0) ||
                (lit < 0 && g_spin_state[(-lit) - 1] < 0)) {
                tc++;
            }
        }
        g_clause_true_cnt[c] = tc;
        if (tc == 0) unsat_insert((uint16_t)c);
    }
}

static inline uint32_t compute_break_count(uint16_t v) {
    uint32_t breaks = 0;
    if (g_spin_state[v] > 0) {
        uint16_t len = g_pos_occ_len[v];
        const uint16_t *arr = g_pos_occ[v];
        for (uint16_t i = 0; i < len; i++) {
            if (g_clause_true_cnt[arr[i]] == 1) breaks++;
        }
    } else {
        uint16_t len = g_neg_occ_len[v];
        const uint16_t *arr = g_neg_occ[v];
        for (uint16_t i = 0; i < len; i++) {
            if (g_clause_true_cnt[arr[i]] == 1) breaks++;
        }
    }
    return breaks;
}

static inline void flip_spin_incremental(uint16_t v) {
    int8_t old_s = g_spin_state[v];
    g_spin_state[v] = (int8_t)(-old_s);

    const uint16_t *gain_arr = (old_s < 0) ? g_pos_occ[v] : g_neg_occ[v];
    uint16_t gain_len        = (old_s < 0) ? g_pos_occ_len[v] : g_neg_occ_len[v];
    const uint16_t *loss_arr = (old_s < 0) ? g_neg_occ[v] : g_pos_occ[v];
    uint16_t loss_len        = (old_s < 0) ? g_neg_occ_len[v] : g_pos_occ_len[v];

    for (uint16_t i = 0; i < gain_len; i++) {
        uint16_t c = gain_arr[i];
        if (g_clause_true_cnt[c] == 0) unsat_remove(c);
        g_clause_true_cnt[c]++;
    }
    for (uint16_t i = 0; i < loss_len; i++) {
        uint16_t c = loss_arr[i];
        g_clause_true_cnt[c]--;
        if (g_clause_true_cnt[c] == 0) unsat_insert(c);
    }
}

static uint32_t count_ising_violations(void) {
    uint32_t unsat_j = 0;
    for (uint32_t i = 0; i < g_num_couplings; i++) {
        const XenoCoupling2 *cp = &g_couplings[i];
        int prod = (int)g_spin_state[cp->var_i] * (int)g_spin_state[cp->var_j];
        if ((cp->weight_j > 0 && prod < 0) || (cp->weight_j < 0 && prod > 0)) {
            unsat_j += (uint32_t)(cp->weight_j > 0 ? cp->weight_j : -cp->weight_j);
        }
    }
    return unsat_j;
}

static void crystallize_vacuum_ground_state(void) {
    set_oscillators_active(1);
    // Brief spin (~40 us) to ensure fresh cross-cluster race ring mixing
    for (volatile int w = 0; w < 400; w++) {}

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    for (uint16_t v = 0; v < g_max_var_plus_one; v++) {
        if (!g_var_bound[v]) continue;
        if (g_var_pinned[v] != 0) {
            g_spin_state[v] = g_var_pinned[v];
        } else if (!g_var_observed[v]) {
            g_spin_state[v] = (harvest_race_fast() & 1U) ? +1 : -1;
        }
    }
    rebuild_incremental_state();

    uint64_t flips = 0;
    const uint32_t max_steps = (g_max_var_plus_one > 400) ? 1500000U : 250000U;
    const uint32_t restart_period = (g_max_var_plus_one > 400) ? 350000U : 25000U;
    // ProbSAT critical 3-SAT Boltzmann polynomial weights ~ 2.06^(-break_count) * 1024
    static const uint32_t k_probsat_weight[16] = {
        4096, 1800, 820, 380, 175, 82, 38, 18, 9, 4, 2, 1, 1, 1, 1, 1
    };

    for (uint32_t step = 0; step < max_steps; step++) {
        uint32_t ising_err = (g_num_couplings > 0) ? count_ising_violations() : 0;
        uint32_t total_err = g_num_unsat_clauses + ising_err;
        if (total_err == 0) {
            g_stat_last_unsat = 0;
            g_ground_state_valid = 1;
            break;
        }
        g_stat_last_unsat = total_err;

        if (step > 0 && (step % restart_period) == 0) {
            for (uint16_t v = 0; v < g_max_var_plus_one; v++) {
                if (g_var_bound[v] && !g_var_observed[v] && g_var_pinned[v] == 0) {
                    g_spin_state[v] = (harvest_race_fast() & 1U) ? +1 : -1;
                }
            }
            rebuild_incremental_state();
            continue;
        }

        uint8_t thermal = harvest_race_fast();

        if (g_num_unsat_clauses > 0) {
            uint32_t r_idx = ((uint32_t)thermal << 8) | (uint32_t)harvest_race_fast();
            uint16_t c_idx = g_unsat_list[r_idx % g_num_unsat_clauses];
            const XenoClause3 *cl = &g_clauses[c_idx];

            uint16_t cand_v[3];
            uint32_t cand_w[3];
            int      cand_n = 0;
            uint32_t w_sum  = 0;

            for (int slot = 0; slot < 3; slot++) {
                int16_t lit = cl->lit[slot];
                uint16_t v = (uint16_t)((lit > 0 ? lit : -lit) - 1);
                if (!g_var_bound[v] || g_var_observed[v] || g_var_pinned[v] != 0) continue;
                uint32_t brk = compute_break_count(v);
                uint32_t w = (brk < 16U) ? k_probsat_weight[brk] : 1U;
                cand_v[cand_n] = v;
                cand_w[cand_n] = w;
                w_sum += w;
                cand_n++;
            }

            if (cand_n == 0) {
                break; // All variables in unsatisfied clause are pinned/observed -> UNSAT!
            }

            uint32_t draw = (((uint32_t)harvest_race_fast() << 8) | (uint32_t)harvest_race_fast()) % w_sum;
            uint16_t chosen = cand_v[cand_n - 1];
            uint32_t acc_w = 0;
            for (int i = 0; i < cand_n; i++) {
                acc_w += cand_w[i];
                if (draw < acc_w) {
                    chosen = cand_v[i];
                    break;
                }
            }
            flip_spin_incremental(chosen);
            flips++;
        } else {
            int progressed = 0;
            for (uint32_t i = 0; i < g_num_couplings; i++) {
                const XenoCoupling2 *cp = &g_couplings[i];
                int prod = (int)g_spin_state[cp->var_i] * (int)g_spin_state[cp->var_j];
                if ((cp->weight_j > 0 && prod < 0) || (cp->weight_j < 0 && prod > 0)) {
                    uint16_t cand = (thermal & 1U) ? cp->var_i : cp->var_j;
                    if (g_var_observed[cand] || g_var_pinned[cand] != 0) {
                        cand = (cand == cp->var_i) ? cp->var_j : cp->var_i;
                    }
                    if (g_var_bound[cand] && !g_var_observed[cand] && g_var_pinned[cand] == 0) {
                        flip_spin_incremental(cand);
                        flips++;
                        progressed = 1;
                        break;
                    }
                }
            }
            if (!progressed) break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    g_stat_total_us += (double)(ts1.tv_sec - ts0.tv_sec) * 1e6 + (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e3;
    atomic_fetch_add_explicit(&g_stat_total_flips, flips, memory_order_relaxed);

    // Park oscillator threads immediately back to 0.0% idle CPU!
    g_anneal_attempted = 1;
    set_oscillators_active(0);
}

// -----------------------------------------------------------------------------
// Coordinator Thread (Blocked on Pipe at 0.0% Idle CPU Until Page Fault)
// -----------------------------------------------------------------------------

static int find_bound_var_at_offset(size_t offset, uint16_t *out_var) {
    for (uint16_t v = 0; v < g_max_var_plus_one; v++) {
        if (g_var_bound[v] && g_var_offset[v] == offset) {
            *out_var = v;
            return 1;
        }
    }
    return 0;
}

static void *coordinator_loop(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    TrapRequest req;
    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        ssize_t n = read(g_req_pipe[0], &req, sizeof(req));
        if (n != (ssize_t)sizeof(req)) {
            if (errno == EINTR) continue;
            break;
        }
        if (req.fault_addr == 0) break;

        TrapReply rep = {0, 1};
        uintptr_t page_base = req.fault_addr & ~(uintptr_t)(g_page_size - 1);
        size_t exact_offset = (size_t)(req.fault_addr - (uintptr_t)g_vacuum_base);

        if (req.is_write) {
            uint16_t hit_var = 0;
            if (find_bound_var_at_offset(exact_offset, &hit_var)) {
                // Direct C pointer write `reality[offset] = val` on a bound variable!
                xeno_impose_measurement(hit_var, (int8_t)req.written_byte);
                rep.advance_pc = 1; // Skip faulting store instruction since spin is now pinned!
            } else {
                // Write to unbound scratch page -> map READ|WRITE and let CPU execute store normally!
                if (mprotect((void *)page_base, g_page_size, PROT_READ | PROT_WRITE) != 0) {
                    rep.ok = 0;
                } else {
                    mark_page_cached_materialized(page_base);
                }
            }
        } else {
            // Read fault (`reality[offset]`)
            if (!page_is_cached_materialized(page_base)) {
                if (!g_ground_state_valid && !g_anneal_attempted) {
                    crystallize_vacuum_ground_state();
                }

                memset(g_staged_page, 0, g_page_size);
                size_t page_off = (size_t)(page_base - (uintptr_t)g_vacuum_base);
                for (uint16_t v = 0; v < g_max_var_plus_one; v++) {
                    if (!g_var_bound[v]) continue;
                    size_t off = g_var_offset[v];
                    if (off >= page_off && off < page_off + g_page_size) {
                        g_staged_page[off - page_off] = (uint8_t)g_spin_state[v];
                        if (g_ground_state_valid) {
                            g_var_observed[v] = 1;
                        }
                    }
                }

                if (mprotect((void *)page_base, g_page_size, PROT_READ | PROT_WRITE) != 0) {
                    rep.ok = 0;
                } else {
                    memcpy((void *)page_base, g_staged_page, g_page_size);
                    if (mprotect((void *)page_base, g_page_size, PROT_READ) != 0) {
                        rep.ok = 0;
                    } else {
                        mark_page_cached_materialized(page_base);
                        atomic_fetch_add_explicit(&g_stat_collapses, 1, memory_order_relaxed);
                    }
                }
            }
        }

        ssize_t wn;
        do {
            wn = write(g_ack_pipe[1], &rep, sizeof(rep));
        } while (wn < 0 && errno == EINTR);
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Async-Signal-Safe, Multi-Thread-Serialized ARM64 MMU Trap Handler
// -----------------------------------------------------------------------------

static void xeno_mmu_observer_trap(int sig, siginfo_t *info, void *uap) {
    uintptr_t addr = (uintptr_t)info->si_addr;
    uintptr_t vac_start = (uintptr_t)g_vacuum_base;
    if (!g_vacuum_base || addr < vac_start || addr >= vac_start + g_vacuum_size) {
        signal(sig, SIG_DFL);
        return;
    }

    int is_write = 0;
    int32_t written_byte = 0;

#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)uap;
    uint32_t esr = uc->uc_mcontext->__es.__esr;
    if ((esr & (1U << 6)) != 0) { // ESR_EL1.WnR == 1 (Write fault)
        is_write = 1;
        uint32_t insn = *(const uint32_t *)(uc->uc_mcontext->__ss.__pc);
        uint32_t rt = insn & 0x1FU;
        uint64_t reg_val = (rt == 31U) ? 0ULL : uc->uc_mcontext->__ss.__x[rt];
        written_byte = (int32_t)(int8_t)(reg_val & 0xFFU);
    }
#else
    (void)uap;
#endif

    // Serialize concurrent multi-thread SIGBUS faults safely
    while (atomic_flag_test_and_set_explicit(&g_trap_spinlock, memory_order_acquire)) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    TrapRequest req = {addr, is_write, written_byte};
    TrapReply rep = {0, 0};

    ssize_t wn;
    do {
        wn = write(g_req_pipe[1], &req, sizeof(req));
    } while (wn < 0 && errno == EINTR);

    ssize_t rn;
    do {
        rn = read(g_ack_pipe[0], &rep, sizeof(rep));
    } while (rn < 0 && errno == EINTR);

    atomic_flag_clear_explicit(&g_trap_spinlock, memory_order_release);

    if (!rep.ok) _exit(99);

#if defined(__aarch64__)
    if (rep.advance_pc) {
        ((ucontext_t *)uap)->uc_mcontext->__ss.__pc += 4;
    }
#endif
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

int xeno_init(size_t virtual_universe_bytes, int8_t **out_vacuum_ptr) {
    g_page_size = (size_t)getpagesize();
    g_vacuum_size = ((virtual_universe_bytes + g_page_size - 1) / g_page_size) * g_page_size;

    void *ptr = mmap(NULL, g_vacuum_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ptr == MAP_FAILED) return -1;
    g_vacuum_base = (uint8_t *)ptr;

    if (pipe(g_req_pipe) != 0) return -2;
    if (pipe(g_ack_pipe) != 0) return -3;

    memset(g_page_cache, 0, sizeof(g_page_cache));
    g_current_epoch = 1;
    g_active_materialized_pages = 0;
    xeno_clear_hamiltonian();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = xeno_mmu_observer_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, NULL) != 0) return -4;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) return -5;

    atomic_store_explicit(&g_osc_active, 0, memory_order_release);
    atomic_store_explicit(&g_running, 1, memory_order_release);

    for (uintptr_t i = 0; i < NUM_SUPER_OSCILLATORS; i++) {
        pthread_create(&g_super_threads[i], NULL, oscillator_worker, (void *)i);
    }
    for (uintptr_t i = 0; i < NUM_EFF_OSCILLATORS; i++) {
        pthread_create(&g_eff_threads[i], NULL, oscillator_worker, (void *)(100U + i));
    }
    pthread_create(&g_coord_thread, NULL, coordinator_loop, NULL);

    xeno_measure_silicon_bath_entropy(&g_stat_shannon, &g_stat_ones_pct);

    *out_vacuum_ptr = (int8_t *)g_vacuum_base;
    return 0;
}

void xeno_shutdown(void) {
    if (!g_vacuum_base) return;
    atomic_store_explicit(&g_running, 0, memory_order_release);
    set_oscillators_active(1); // Wake any parked oscillator threads so they exit cleanly

    TrapRequest poison = {0, 0, 0};
    ssize_t wn = write(g_req_pipe[1], &poison, sizeof(poison));
    (void)wn;

    for (int i = 0; i < NUM_SUPER_OSCILLATORS; i++) pthread_join(g_super_threads[i], NULL);
    for (int i = 0; i < NUM_EFF_OSCILLATORS; i++) pthread_join(g_eff_threads[i], NULL);
    pthread_join(g_coord_thread, NULL);

    signal(SIGBUS, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    close(g_req_pipe[0]); close(g_req_pipe[1]);
    close(g_ack_pipe[0]); close(g_ack_pipe[1]);
    munmap(g_vacuum_base, g_vacuum_size);
    g_vacuum_base = NULL;
}

void xeno_clear_hamiltonian(void) {
    invalidate_all_materialized_pages(0);
    g_max_var_plus_one = 0;
    g_num_clauses = 0;
    g_num_couplings = 0;
    g_num_unsat_clauses = 0;
    atomic_store_explicit(&g_stat_total_flips, 0, memory_order_relaxed);
    g_stat_total_us = 0.0;
    memset(g_var_bound, 0, sizeof(g_var_bound));
    memset(g_var_pinned, 0, sizeof(g_var_pinned));
    memset(g_spin_state, 0, sizeof(g_spin_state));
    memset(g_pos_occ_len, 0, sizeof(g_pos_occ_len));
    memset(g_neg_occ_len, 0, sizeof(g_neg_occ_len));
}

int xeno_bind_variable(uint16_t var_id, size_t byte_offset_in_vacuum) {
    if (var_id >= XENO_MAX_VARS || byte_offset_in_vacuum >= g_vacuum_size) return -1;
    g_var_bound[var_id] = 1;
    g_var_offset[var_id] = byte_offset_in_vacuum;
    g_spin_state[var_id] = +1;
    if (var_id + 1 > g_max_var_plus_one) g_max_var_plus_one = var_id + 1;
    g_ground_state_valid = 0;
    g_anneal_attempted = 0;
    return 0;
}

int xeno_add_3sat_clause(int16_t lit0, int16_t lit1, int16_t lit2) {
    if (g_num_clauses >= XENO_MAX_CLAUSES) return -1;
    int16_t lits[3] = {lit0, lit1, lit2};

    // Reject literal 0 or any unbound / out-of-range variable!
    for (int k = 0; k < 3; k++) {
        if (lits[k] == 0) return -1;
        int32_t abs_lit = (lits[k] > 0) ? (int32_t)lits[k] : -(int32_t)lits[k];
        if (abs_lit < 1 || abs_lit > XENO_MAX_VARS) return -1;
        uint16_t v = (uint16_t)(abs_lit - 1);
        if (!g_var_bound[v]) return -1;
    }

    uint16_t cid = (uint16_t)g_num_clauses;
    g_clauses[cid].lit[0] = lit0;
    g_clauses[cid].lit[1] = lit1;
    g_clauses[cid].lit[2] = lit2;

    for (int k = 0; k < 3; k++) {
        int16_t l = lits[k];
        uint16_t v = (uint16_t)((l > 0 ? l : -l) - 1);
        int dup = 0;
        for (int prev = 0; prev < k; prev++) {
            if (lits[prev] == l) dup = 1;
        }
        if (!dup) {
            if (l > 0 && g_pos_occ_len[v] < MAX_VAR_OCCURRENCES) {
                g_pos_occ[v][g_pos_occ_len[v]++] = cid;
            } else if (l < 0 && g_neg_occ_len[v] < MAX_VAR_OCCURRENCES) {
                g_neg_occ[v][g_neg_occ_len[v]++] = cid;
            }
        }
    }

    g_num_clauses++;
    g_ground_state_valid = 0;
    g_anneal_attempted = 0;
    return 0;
}

int xeno_add_ising_coupling(uint16_t var_i, uint16_t var_j, int16_t j_coupling) {
    if (g_num_couplings >= XENO_MAX_CLAUSES) return -1;
    if (var_i >= XENO_MAX_VARS || var_j >= XENO_MAX_VARS) return -1;
    if (!g_var_bound[var_i] || !g_var_bound[var_j]) return -1;
    g_couplings[g_num_couplings].var_i = var_i;
    g_couplings[g_num_couplings].var_j = var_j;
    g_couplings[g_num_couplings].weight_j = j_coupling;
    g_num_couplings++;
    g_ground_state_valid = 0;
    g_anneal_attempted = 0;
    return 0;
}

int xeno_impose_measurement(uint16_t var_id, int8_t pinned_spin) {
    if (var_id >= XENO_MAX_VARS || !g_var_bound[var_id]) return -1;
    g_var_pinned[var_id] = (pinned_spin >= 0) ? +1 : -1;
    g_spin_state[var_id] = g_var_pinned[var_id];

    rebuild_incremental_state();
    uint32_t violations = g_num_unsat_clauses + count_ising_violations();
    int was_observed = g_var_observed[var_id];

    if (violations > 0 || was_observed) {
        invalidate_all_materialized_pages(1);
    }
    return 0;
}

void xeno_measure_entropy_ex(
    int oscillators_active,
    double *out_shannon,
    double *out_min_entropy,
    double *out_markov_cond_entropy,
    double *out_lag1_corr
) {
    if (oscillators_active) {
        set_oscillators_active(1);
        usleep(1500);
    } else {
        set_oscillators_active(0);
    }

    const int N = 4096;
    uint8_t seq[4096];
    uint32_t hist[256] = {0};
    static uint32_t trans[256][256];
    memset(trans, 0, sizeof(trans));

    for (int i = 0; i < N; i++) {
        uint8_t b = sample_entropy_byte(oscillators_active);
        seq[i] = b;
        hist[b]++;
        if (i > 0) trans[seq[i - 1]][b]++;
    }

    if (oscillators_active) {
        set_oscillators_active(0);
    }

    double shannon = 0.0;
    uint32_t max_c = 0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > max_c) max_c = hist[i];
        if (hist[i] > 0) {
            double p = (double)hist[i] / (double)N;
            shannon -= p * log2(p);
        }
    }
    double min_ent = (max_c > 0) ? -log2((double)max_c / (double)N) : 0.0;

    // First-order Markov conditional entropy H(X_t | X_{t-1})
    double markov_h = 0.0;
    for (int x = 0; x < 256; x++) {
        uint32_t row_sum = 0;
        for (int y = 0; y < 256; y++) row_sum += trans[x][y];
        if (row_sum == 0) continue;
        double px = (double)row_sum / (double)(N - 1);
        double row_h = 0.0;
        for (int y = 0; y < 256; y++) {
            if (trans[x][y] > 0) {
                double py_given_x = (double)trans[x][y] / (double)row_sum;
                row_h -= py_given_x * log2(py_given_x);
            }
        }
        markov_h += px * row_h;
    }

    // Lag-1 serial correlation coefficient
    double mean = 0.0;
    for (int i = 0; i < N; i++) mean += (double)seq[i];
    mean /= (double)N;
    double num = 0.0, den = 1e-12;
    for (int i = 0; i < N; i++) {
        double d = (double)seq[i] - mean;
        den += d * d;
        if (i > 0) num += ((double)seq[i - 1] - mean) * d;
    }
    double lag1 = num / den;

    if (out_shannon) *out_shannon = shannon;
    if (out_min_entropy) *out_min_entropy = min_ent;
    if (out_markov_cond_entropy) *out_markov_cond_entropy = markov_h;
    if (out_lag1_corr) *out_lag1_corr = lag1;
}

void xeno_measure_silicon_bath_entropy(double *out_shannon_bits, double *out_ones_pct) {
    set_oscillators_active(1);
    usleep(1000);
    uint32_t hist[256] = {0};
    uint32_t ones = 0;
    const int samples = 4096;
    for (int i = 0; i < samples; i++) {
        uint8_t b = sample_entropy_byte(1);
        hist[b]++;
        for (int k = 0; k < 8; k++) ones += (b >> k) & 1U;
    }
    set_oscillators_active(0);

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
