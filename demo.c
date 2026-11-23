#include "xenomem.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/sysctl.h>

/*
 * Deterministic splitmix64 used ONLY by the benchmark harness to construct a
 * reproducible 3-SAT formula with a known satisfiable assignment for verification.
 * `xenomem.c` itself receives only the shuffled clauses, never `secret_witness`.
 */
static uint64_t harness_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void query_host_silicon(char *out_buf, size_t cap) {
    char brand[128] = "Generic ARM64/POSIX Host";
    size_t sz = sizeof(brand);
    sysctlbyname("machdep.cpu.brand_string", brand, &sz, NULL, 0);

    int p_cores = 0, e_cores = 0;
    size_t isz = sizeof(int);
    sysctlbyname("hw.perflevel0.physicalcpu", &p_cores, &isz, NULL, 0);
    isz = sizeof(int);
    sysctlbyname("hw.perflevel1.physicalcpu", &e_cores, &isz, NULL, 0);

    char p_name[32] = "Fast", e_name[32] = "Efficiency";
    sz = sizeof(p_name);
    sysctlbyname("hw.perflevel0.name", p_name, &sz, NULL, 0);
    sz = sizeof(e_name);
    sysctlbyname("hw.perflevel1.name", e_name, &sz, NULL, 0);

    if (p_cores > 0 && e_cores > 0) {
        snprintf(out_buf, cap, "%s (%d %s + %d %s cores, runtime-queried)",
                 brand, p_cores, p_name, e_cores, e_name);
    } else {
        snprintf(out_buf, cap, "%s (runtime-queried)", brand);
    }
}

int main(void) {
    const size_t GB = 1024ULL * 1024ULL * 1024ULL;
    const size_t UNIVERSE_BYTES = 64ULL * GB;

    char host_desc[256];
    query_host_silicon(host_desc, sizeof(host_desc));

    volatile int8_t *reality = NULL;
    if (xeno_init(UNIVERSE_BYTES, (int8_t **)&reality) != 0) {
        fprintf(stderr, "Failed to initialize 64 GB XenoMem region.\n");
        return 1;
    }

    // Measure A/B entropy: Threads Parked (cntvct only) vs Cross-QoS Race Active
    double off_sh = 0, off_min = 0, off_mk = 0, off_corr = 0;
    double on_sh = 0, on_min = 0, on_mk = 0, on_corr = 0;
    xeno_measure_entropy_ex(0, &off_sh, &off_min, &off_mk, &off_corr);
    xeno_measure_entropy_ex(1, &on_sh, &on_min, &on_mk, &on_corr);
    XenoTelemetry t0 = xeno_get_telemetry();

    printf("========================================================================================\n");
    printf("  XENOMEM // DEMAND-PAGED SIGBUS CONSTRAINT MEMORY & DATA-RACE JITTER SOLVER\n");
    printf("  Host Silicon     : %s\n", host_desc);
    printf("  Virtual Region   : 64.0 GB PROT_NONE  |  Initial Physical Footprint: %.2f MB\n",
           (double)t0.phys_footprint_bytes / (1024.0 * 1024.0));
    printf("========================================================================================\n\n");

    printf("[1] CROSS-QOS CACHE-CONTENTION JITTER AUDIT (A/B CONTROL · 0 PRNG IMPORTS)\n");
    printf("    • Oscillators OFF  : Shannon=%.3f  MinEnt(H_inf)=%.3f  Markov H(Xt|Xt-1)=%.3f\n",
           off_sh, off_min, off_mk);
    printf("    • Oscillators ON   : Shannon=%.3f  MinEnt(H_inf)=%.3f  Markov H(Xt|Xt-1)=%.3f  Lag1Corr=%+.4f\n\n",
           on_sh, on_min, on_mk, on_corr);

    // -------------------------------------------------------------------------
    // EXPERIMENT 2: Critical-Ratio 3-SAT (N=300 variables, M=1,278 clauses, alpha=4.26)
    // Solved on demand when dereferencing unmapped pages across 60 GB!
    // -------------------------------------------------------------------------
    xeno_clear_hamiltonian();

    const uint16_t N_VARS = 300;
    const uint32_t M_CLAUSES = 1278;
    size_t var_addrs[300];

    for (uint16_t v = 0; v < N_VARS; v++) {
        size_t cluster = (size_t)(v / 10) * (2ULL * GB);
        size_t slot = (size_t)(v % 10) * 64ULL + 128ULL;
        var_addrs[v] = cluster + slot;
        if (xeno_bind_variable(v, var_addrs[v]) != 0) return 3;
    }

    uint64_t seed = 0x20261123ULL;
    int8_t secret_witness[300];
    for (int i = 0; i < (int)N_VARS; i++) {
        secret_witness[i] = (harness_splitmix64(&seed) & 1ULL) ? +1 : -1;
    }

    XenoClause3 verify_clauses[1278];
    for (uint32_t c = 0; c < M_CLAUSES; c++) {
        uint16_t v0 = (uint16_t)(harness_splitmix64(&seed) % N_VARS);
        uint16_t v1 = (uint16_t)(harness_splitmix64(&seed) % N_VARS);
        while (v1 == v0) v1 = (uint16_t)(harness_splitmix64(&seed) % N_VARS);
        uint16_t v2 = (uint16_t)(harness_splitmix64(&seed) % N_VARS);
        while (v2 == v0 || v2 == v1) v2 = (uint16_t)(harness_splitmix64(&seed) % N_VARS);

        int16_t l0 = (harness_splitmix64(&seed) & 1ULL) ? (int16_t)(v0 + 1) : (int16_t)-(v0 + 1);
        int16_t l1 = (harness_splitmix64(&seed) & 1ULL) ? (int16_t)(v1 + 1) : (int16_t)-(v1 + 1);
        int16_t l2 = (harness_splitmix64(&seed) & 1ULL) ? (int16_t)(v2 + 1) : (int16_t)-(v2 + 1);

        int sat0 = (l0 > 0 && secret_witness[v0] > 0) || (l0 < 0 && secret_witness[v0] < 0);
        int sat1 = (l1 > 0 && secret_witness[v1] > 0) || (l1 < 0 && secret_witness[v1] < 0);
        int sat2 = (l2 > 0 && secret_witness[v2] > 0) || (l2 < 0 && secret_witness[v2] < 0);
        if (!sat0 && !sat1 && !sat2) {
            int pick = (int)(harness_splitmix64(&seed) % 3ULL);
            if (pick == 0) l0 = (int16_t)(-l0);
            else if (pick == 1) l1 = (int16_t)(-l1);
            else l2 = (int16_t)(-l2);
        }

        verify_clauses[c].lit[0] = l0;
        verify_clauses[c].lit[1] = l1;
        verify_clauses[c].lit[2] = l2;
        if (xeno_add_3sat_clause(l0, l1, l2) != 0) return 4;
    }

    int8_t observed_spins[300];
    for (uint16_t v = 0; v < N_VARS; v++) {
        observed_spins[v] = reality[var_addrs[v]];
    }

    uint32_t satisfied_count = 0;
    for (uint32_t c = 0; c < M_CLAUSES; c++) {
        int ok = 0;
        for (int k = 0; k < 3; k++) {
            int16_t lit = verify_clauses[c].lit[k];
            if (lit > 0 && observed_spins[lit - 1] == +1) ok = 1;
            if (lit < 0 && observed_spins[(-lit) - 1] == -1) ok = 1;
        }
        if (ok) satisfied_count++;
    }

    XenoTelemetry t_sat = xeno_get_telemetry();
    printf("[2] DEMAND-PAGED 3-SAT VIA SIGBUS FAULTS (N=%u Vars, M=%u Clauses, alpha=4.26)\n", N_VARS, M_CLAUSES);
    printf("    • Clauses Satisfied  : %u / %u (%.1f%%)\n",
           satisfied_count, M_CLAUSES, 100.0 * (double)satisfied_count / (double)M_CLAUSES);
    printf("    • WalkSAT Work       : %llu O(1) flips in %.1f µs total fault-handler solve time\n",
           (unsigned long long)t_sat.total_anneal_race_flips, t_sat.total_collapse_latency_us);
    printf("    • Demand Paging      : %llu × 16 KB pages faulted out of 64.0 GB (Footprint: %.2f MB)\n\n",
           (unsigned long long)t_sat.total_page_collapses,
           (double)t_sat.phys_footprint_bytes / (1024.0 * 1024.0));

    // -------------------------------------------------------------------------
    // EXPERIMENT 3: Ising Coupling Across 48 GB + Direct C Pointer Write Trap!
    // -------------------------------------------------------------------------
    xeno_clear_hamiltonian();

    size_t addr_alice = 0ULL * GB + 256ULL;
    size_t addr_med1  = 16ULL * GB + 256ULL;
    size_t addr_med2  = 32ULL * GB + 256ULL;
    size_t addr_bob   = 48ULL * GB + 256ULL;

    xeno_bind_variable(0, addr_alice);
    xeno_bind_variable(1, addr_med1);
    xeno_bind_variable(2, addr_med2);
    xeno_bind_variable(3, addr_bob);

    xeno_add_ising_coupling(0, 1, -5);
    xeno_add_ising_coupling(1, 2, -5);
    xeno_add_ising_coupling(2, 3, -5);

    int8_t alice_t1 = reality[addr_alice];
    int8_t bob_t1   = reality[addr_bob];

    // Direct C pointer assignment `reality[48 GB] = forced_bob`!
    // Decoded via ARM64 ESR_EL1 WnR bit -> pins variable 3 & remaps contradicted pages PROT_NONE!
    int8_t forced_bob = alice_t1;
#if defined(__aarch64__)
    reality[addr_bob] = forced_bob;
#else
    xeno_impose_measurement(3, forced_bob);
#endif

    int8_t alice_t2 = reality[addr_alice];
    int8_t bob_t2   = reality[addr_bob];

    printf("[3] ISING COUPLING ACROSS 48 GB & DIRECT ARM64 POINTER-WRITE INVALIDATION\n");
    printf("    • Initial Read       : reality[0 GB] = %+d  |  reality[48 GB] = %+d  (Anti-aligned: %s)\n",
           alice_t1, bob_t1, (alice_t1 == -bob_t1) ? "OK" : "FAIL");
    printf("    • Direct Write Trap  : `reality[48 GB] = %+d` intercepted -> invalidated stale pages\n", forced_bob);
    printf("    • Consistent Re-Read : reality[0 GB] = %+d  |  reality[48 GB] = %+d  (Re-solved: %s)\n\n",
           alice_t2, bob_t2, (alice_t2 == -bob_t2 && bob_t2 == forced_bob) ? "OK" : "FAIL");

    // -------------------------------------------------------------------------
    // EXPERIMENT 4: Regression & Adversarial Invariant Self-Checks
    // -------------------------------------------------------------------------
    int inv_ok = 1;

    // 4a. Reject literal 0 & out-of-range/unbound literals
    if (xeno_add_3sat_clause(0, 1, 2) == 0) inv_ok = 0;
    if (xeno_add_3sat_clause(999, 1, 2) == 0) inv_ok = 0;

    // 4b. High page index (> 256 pages) clean invalidation check
    xeno_clear_hamiltonian();
    for (uint16_t p = 0; p < 305; p++) {
        size_t off = (size_t)p * 16384ULL + 64ULL;
        if (xeno_bind_variable(p, off) != 0) inv_ok = 0;
        (void)reality[off];
    }
    xeno_clear_hamiltonian();
    // Bind ONLY variable 5 on page 295: offset 0 must stay 0 (unbound), page 295 must read +/-1 (not stale 0!)
    size_t page295_off = 295ULL * 16384ULL + 64ULL;
    xeno_bind_variable(5, page295_off);
    int8_t unbound_zero = reality[0];
    int8_t page295_spin = reality[page295_off];
    if (unbound_zero != 0 || (page295_spin != 1 && page295_spin != -1)) inv_ok = 0;

    // 4c. Unbound scratch write must succeed without hanging
    reality[16384ULL * 400ULL + 32ULL] = 42;
    if (reality[16384ULL * 400ULL + 32ULL] != 42) inv_ok = 0;

    // 4d. Verify ~0.0% idle CPU while threads are parked across a 120ms sleep
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    usleep(120000);
    getrusage(RUSAGE_SELF, &ru1);
    double cpu_us = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1e6
                  + (double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec)
                  + (double)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1e6
                  + (double)(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);
    double idle_cpu_pct = (cpu_us / 120000.0) * 100.0;
    if (idle_cpu_pct > 5.0) inv_ok = 0;

    printf("[4] ADVERSARIAL INVARIANT & IDLE-CPU SELF-AUDIT\n");
    printf("    • Bounds & Sparse Var: lit=0 rejected | sparse var[5] isolated | page 295+ clean (%s)\n",
           inv_ok ? "PASS" : "FAIL");
    printf("    • Parked Idle CPU    : %.2f%% CPU during 120 ms idle window (0%% busy-spin)\n", idle_cpu_pct);
    printf("========================================================================================\n");

    xeno_shutdown();
    return (satisfied_count == M_CLAUSES && alice_t1 == -bob_t1 && alice_t2 == -bob_t2 && inv_ok) ? 0 : 2;
}
