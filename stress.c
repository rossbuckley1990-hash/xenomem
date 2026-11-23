#include "xenomem.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// Deterministic Weyl mixer ONLY for generating reproducible hard benchmark formulas in the harness
static uint64_t harness_mix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    volatile int8_t *reality;
    const size_t    *offsets;
    uint16_t         start_idx;
    uint16_t         end_idx;
    int8_t          *out_vals;
} ReaderTask;

static void *concurrent_observer_worker(void *arg) {
    ReaderTask *t = (ReaderTask *)arg;
    for (uint16_t i = t->start_idx; i < t->end_idx; i++) {
        t->out_vals[i] = t->reality[t->offsets[i]];
    }
    return NULL;
}

int main(void) {
    const size_t GB = 1024ULL * 1024ULL * 1024ULL;
    const size_t VOID_BYTES = 256ULL * GB; // Quarter-Terabyte (256.0 GB) PROT_NONE void!

    volatile int8_t *reality = NULL;
    if (xeno_init(VOID_BYTES, (int8_t **)&reality) != 0) {
        fprintf(stderr, "FAIL: xeno_init(256 GB) failed\n");
        return 1;
    }

    printf("========================================================================================\n");
    printf("  XENOMEM TORTURE & STRESS SUITE (256.0 GB PROT_NONE VOID · ZERO PRNG)\n");
    printf("========================================================================================\n\n");

    int failures = 0;

    // -------------------------------------------------------------------------
    // STRESS 1: Silicon Race Bath Statistical Torture Under Load (64 Trials = 512 KB)
    // -------------------------------------------------------------------------
    double min_h = 8.0, max_h = 0.0, sum_h = 0.0;
    double min_ones = 100.0, max_ones = 0.0;
    double t0 = now_ms();
    for (int i = 0; i < 64; i++) {
        double h = 0.0, ones = 0.0;
        xeno_measure_silicon_bath_entropy(&h, &ones);
        if (h < min_h) min_h = h;
        if (h > max_h) max_h = h;
        sum_h += h;
        if (ones < min_ones) min_ones = ones;
        if (ones > max_ones) max_ones = ones;
    }
    double dt_ent = now_ms() - t0;
    int ent_ok = (min_h > 7.50 && min_ones > 46.0 && max_ones < 54.0);
    if (!ent_ok) failures++;
    printf("[STRESS 1] 524,288-Byte Zero-PRNG Silicon Contention Entropy Battery (%.1f ms)\n", dt_ent);
    printf("    • Mean Entropy : %.4f / 8.0000 bits/byte  (Min: %.4f, Max: %.4f)\n",
           sum_h / 64.0, min_h, max_h);
    printf("    • Bit Balance  : [%.2f%% .. %.2f%%] ones  -> %s\n\n",
           min_ones, max_ones, ent_ok ? "PASS" : "FAIL");

    // -------------------------------------------------------------------------
    // STRESS 2: 20 Consecutive Critical Phase-Transition 3-SAT Trials (alpha = 4.25)
    // -------------------------------------------------------------------------
    const uint16_t N_MED = 250;
    const uint32_t M_MED = 1062; // 250 * 4.25 = 1062.5 critical hardness!
    int solved_trials = 0;
    double total_med_ms = 0.0;
    uint64_t total_med_flips = 0;

    for (int trial = 0; trial < 20; trial++) {
        xeno_clear_hamiltonian();
        size_t offs[250];
        for (uint16_t v = 0; v < N_MED; v++) {
            offs[v] = (size_t)v * (1ULL * GB) + 512ULL; // 250 pages across 250 GB!
            xeno_bind_variable(v, offs[v]);
        }

        uint64_t seed = 0xABCDEF0000ULL + (uint64_t)trial * 1337ULL;
        int8_t witness[250];
        for (uint16_t v = 0; v < N_MED; v++) witness[v] = (harness_mix(&seed) & 1ULL) ? +1 : -1;

        XenoClause3 clauses[1062];
        for (uint32_t c = 0; c < M_MED; c++) {
            uint16_t v0 = (uint16_t)(harness_mix(&seed) % N_MED);
            uint16_t v1 = (uint16_t)(harness_mix(&seed) % N_MED);
            while (v1 == v0) v1 = (uint16_t)(harness_mix(&seed) % N_MED);
            uint16_t v2 = (uint16_t)(harness_mix(&seed) % N_MED);
            while (v2 == v0 || v2 == v1) v2 = (uint16_t)(harness_mix(&seed) % N_MED);

            int16_t l0 = (harness_mix(&seed) & 1ULL) ? (int16_t)(v0 + 1) : (int16_t)-(v0 + 1);
            int16_t l1 = (harness_mix(&seed) & 1ULL) ? (int16_t)(v1 + 1) : (int16_t)-(v1 + 1);
            int16_t l2 = (harness_mix(&seed) & 1ULL) ? (int16_t)(v2 + 1) : (int16_t)-(v2 + 1);

            int s0 = (l0 > 0 && witness[v0] > 0) || (l0 < 0 && witness[v0] < 0);
            int s1 = (l1 > 0 && witness[v1] > 0) || (l1 < 0 && witness[v1] < 0);
            int s2 = (l2 > 0 && witness[v2] > 0) || (l2 < 0 && witness[v2] < 0);
            if (!s0 && !s1 && !s2) {
                int p = (int)(harness_mix(&seed) % 3ULL);
                if (p == 0) l0 = (int16_t)(-l0);
                else if (p == 1) l1 = (int16_t)(-l1);
                else l2 = (int16_t)(-l2);
            }
            clauses[c].lit[0] = l0; clauses[c].lit[1] = l1; clauses[c].lit[2] = l2;
            xeno_add_3sat_clause(l0, l1, l2);
        }

        double ts = now_ms();
        int8_t obs[250];
        for (uint16_t v = 0; v < N_MED; v++) obs[v] = reality[offs[v]];
        total_med_ms += (now_ms() - ts);

        uint32_t sat = 0;
        for (uint32_t c = 0; c < M_MED; c++) {
            int ok = 0;
            for (int k = 0; k < 3; k++) {
                int16_t l = clauses[c].lit[k];
                if ((l > 0 && obs[l - 1] == +1) || (l < 0 && obs[(-l) - 1] == -1)) ok = 1;
            }
            if (ok) sat++;
        }
        if (sat == M_MED) solved_trials++;
        total_med_flips += xeno_get_telemetry().total_anneal_race_flips;
    }
    if (solved_trials < 20) failures++;
    printf("[STRESS 2] 20x Critical 3-SAT Marathon Across 250 GB (N=250, M=1,062, α=4.25)\n");
    printf("    • Ground States : %d / 20 solved (100%% of 21,240 clauses) in %.1f ms total (%.2f ms/instance)\n",
           solved_trials, total_med_ms, total_med_ms / 20.0);
    printf("    • Total Flips   : %llu zero-PRNG silicon race flips  -> %s\n\n",
           (unsigned long long)total_med_flips, (solved_trials == 20) ? "PASS" : "FAIL");

    // -------------------------------------------------------------------------
    // STRESS 3: Large-Scale N=1,000 Spins, M=4,250 Clauses Across 1,000 Pages!
    // -------------------------------------------------------------------------
    xeno_clear_hamiltonian();
    const uint16_t N_BIG = 1000;
    const uint32_t M_BIG = 4250;
    static size_t big_offs[1000];
    for (uint16_t v = 0; v < N_BIG; v++) {
        big_offs[v] = (size_t)v * (256ULL * 1024ULL * 1024ULL) + 1024ULL; // 1,000 pages across 250 GB!
        if (xeno_bind_variable(v, big_offs[v]) != 0) {
            printf("[STRESS 3] Bind failed at var %u (exceeded XENO_MAX_VARS!)\n", v);
            failures++;
            break;
        }
    }

    uint64_t bseed = 0x9988776655443322ULL;
    static int8_t bwitness[1000];
    for (uint16_t v = 0; v < N_BIG; v++) bwitness[v] = (harness_mix(&bseed) & 1ULL) ? +1 : -1;

    static XenoClause3 bclauses[4250];
    for (uint32_t c = 0; c < M_BIG; c++) {
        uint16_t v0 = (uint16_t)(harness_mix(&bseed) % N_BIG);
        uint16_t v1 = (uint16_t)(harness_mix(&bseed) % N_BIG);
        while (v1 == v0) v1 = (uint16_t)(harness_mix(&bseed) % N_BIG);
        uint16_t v2 = (uint16_t)(harness_mix(&bseed) % N_BIG);
        while (v2 == v0 || v2 == v1) v2 = (uint16_t)(harness_mix(&bseed) % N_BIG);

        int16_t l0 = (harness_mix(&bseed) & 1ULL) ? (int16_t)(v0 + 1) : (int16_t)-(v0 + 1);
        int16_t l1 = (harness_mix(&bseed) & 1ULL) ? (int16_t)(v1 + 1) : (int16_t)-(v1 + 1);
        int16_t l2 = (harness_mix(&bseed) & 1ULL) ? (int16_t)(v2 + 1) : (int16_t)-(v2 + 1);

        int s0 = (l0 > 0 && bwitness[v0] > 0) || (l0 < 0 && bwitness[v0] < 0);
        int s1 = (l1 > 0 && bwitness[v1] > 0) || (l1 < 0 && bwitness[v1] < 0);
        int s2 = (l2 > 0 && bwitness[v2] > 0) || (l2 < 0 && bwitness[v2] < 0);
        if (!s0 && !s1 && !s2) {
            int p = (int)(harness_mix(&bseed) % 3ULL);
            if (p == 0) l0 = (int16_t)(-l0);
            else if (p == 1) l1 = (int16_t)(-l1);
            else l2 = (int16_t)(-l2);
        }
        bclauses[c].lit[0] = l0; bclauses[c].lit[1] = l1; bclauses[c].lit[2] = l2;
        xeno_add_3sat_clause(l0, l1, l2);
    }

    // -------------------------------------------------------------------------
    // STRESS 4: 16 Concurrent Observer Threads Dereferencing the 1,000 Pages Simultaneously!
    // -------------------------------------------------------------------------
    static int8_t bobs[1000];
    pthread_t rthreads[16];
    ReaderTask rtasks[16];
    double t_mt0 = now_ms();
    for (int t = 0; t < 16; t++) {
        rtasks[t].reality = reality;
        rtasks[t].offsets = big_offs;
        rtasks[t].start_idx = (uint16_t)(t * 62);
        rtasks[t].end_idx = (uint16_t)((t == 15) ? N_BIG : (t + 1) * 62);
        rtasks[t].out_vals = bobs;
        pthread_create(&rthreads[t], NULL, concurrent_observer_worker, &rtasks[t]);
    }
    for (int t = 0; t < 16; t++) pthread_join(rthreads[t], NULL);
    double dt_mt = now_ms() - t_mt0;

    uint32_t bsat = 0;
    for (uint32_t c = 0; c < M_BIG; c++) {
        int ok = 0;
        for (int k = 0; k < 3; k++) {
            int16_t l = bclauses[c].lit[k];
            if ((l > 0 && bobs[l - 1] == +1) || (l < 0 && bobs[(-l) - 1] == -1)) ok = 1;
        }
        if (ok) bsat++;
    }
    if (bsat != M_BIG) failures++;
    XenoTelemetry t_big = xeno_get_telemetry();
    printf("[STRESS 3 & 4] 16-Thread Concurrent SIGBUS Storm on N=1,000, M=4,250 Across 1,000 Pages\n");
    printf("    • Clauses Solved : %u / %u (%.2f%%) in %.2f ms across 16 racing reader threads!\n",
           bsat, M_BIG, 100.0 * (double)bsat / (double)M_BIG, dt_mt);
    printf("    • Flips & RAM    : %llu race flips | Footprint: %.2f MB out of 256.0 GB -> %s\n\n",
           (unsigned long long)t_big.total_anneal_race_flips,
           (double)t_big.phys_footprint_bytes / (1024.0 * 1024.0),
           (bsat == M_BIG) ? "PASS" : "FAIL");

    // -------------------------------------------------------------------------
    // STRESS 5: 250 Rapid-Fire Retrocausal Vacuum-Decay Paradox Cycles Across 240 GB
    // -------------------------------------------------------------------------
    xeno_clear_hamiltonian();
    const int CHAIN = 32;
    size_t chain_offs[32];
    for (int i = 0; i < CHAIN; i++) {
        chain_offs[i] = (size_t)i * (7ULL * GB) + 4096ULL; // Spans 217 GB across 32 pages!
        xeno_bind_variable((uint16_t)i, chain_offs[i]);
        if (i > 0) xeno_add_ising_coupling((uint16_t)(i - 1), (uint16_t)i, -5); // Alternating antiferromagnetic chain
    }

    int decay_passes = 0;
    double t_vd0 = now_ms();
    for (int cycle = 0; cycle < 250; cycle++) {
        int8_t head = reality[chain_offs[0]];
        int8_t tail = reality[chain_offs[CHAIN - 1]];
        // 31 anti-ferromagnetic links -> tail MUST equal -head!
        if (tail != -head) break;

        // Inject paradox via direct ARM64 pointer write (`reality[tail] = head`) on even cycles,
        // and API measurement on odd cycles!
#if defined(__aarch64__)
        if ((cycle & 1) == 0) {
            reality[chain_offs[CHAIN - 1]] = head;
        } else {
            xeno_impose_measurement((uint16_t)(CHAIN - 1), head);
        }
#else
        xeno_impose_measurement((uint16_t)(CHAIN - 1), head);
#endif

        // Re-read entire 32-node chain across 217 GB without writing to any pointer!
        int chain_valid = 1;
        int8_t prev = reality[chain_offs[0]];
        if (prev != -head) chain_valid = 0;
        for (int i = 1; i < CHAIN; i++) {
            int8_t cur = reality[chain_offs[i]];
            if (cur != -prev) chain_valid = 0;
            prev = cur;
        }
        if (chain_valid) decay_passes++;
    }
    double dt_vd = now_ms() - t_vd0;
    if (decay_passes != 250) failures++;
    XenoTelemetry t_vd = xeno_get_telemetry();
    printf("[STRESS 5] 250 Rapid-Fire Retrocausal Vacuum-Decay Cycles Across 32-Node 217 GB Chain\n");
    printf("    • Causal Healing : %d / 250 paradox cycles self-healed across all 32 pages in %.2f ms\n",
           decay_passes, dt_vd);
    printf("    • Vacuum Decays  : %llu page dematerializations -> %s\n\n",
           (unsigned long long)t_vd.total_vacuum_decays, (decay_passes == 250) ? "PASS" : "FAIL");

    // -------------------------------------------------------------------------
    // STRESS 6: Impossible Contradiction (UNSAT) Bounded Termination Safety
    // -------------------------------------------------------------------------
    xeno_clear_hamiltonian();
    xeno_bind_variable(0, 64ULL);
    xeno_bind_variable(1, 128ULL);
    // All 8 sign combinations of (v0, v1, v0) forbidden -> provably UNSAT!
    xeno_add_3sat_clause(+1, +1, +1);
    xeno_add_3sat_clause(-1, -1, -1);

    double t_un0 = now_ms();
    volatile int8_t unsat_read = reality[64ULL];
    (void)unsat_read;
    double dt_un = now_ms() - t_un0;
    XenoTelemetry t_un = xeno_get_telemetry();
    int unsat_ok = (t_un.last_unsatisfied_clauses > 0 && dt_un < 2000.0);
    if (!unsat_ok) failures++;
    printf("[STRESS 6] Provably Impossible Contradiction (x AND NOT x) Non-Hang Safety\n");
    printf("    • Bounded Return : Returned cleanly in %.2f ms with unsatisfied_clauses=%u -> %s\n",
           dt_un, t_un.last_unsatisfied_clauses, unsat_ok ? "PASS" : "FAIL");

    printf("========================================================================================\n");
    printf("  STRESS VERDICT: %s (%d failures)  |  Final Footprint: %.2f MB / 256.0 GB Void\n",
           (failures == 0) ? "ALL 6 TORTURE STAGES PASSED" : "BROKEN UNDER STRESS",
           failures, (double)t_un.phys_footprint_bytes / (1024.0 * 1024.0));
    printf("========================================================================================\n");

    xeno_shutdown();
    return (failures == 0) ? 0 : 2;
}
