#ifndef XENOMEM_H
#define XENOMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum variables and clauses in a coupled constraint manifold
#define XENO_MAX_VARS    512
#define XENO_MAX_CLAUSES 4096

// A 3-SAT / Ising 3-body clause: (lit0 OR lit1 OR lit2)
// Positive literal +k means spin[k-1] == +1; negative literal -k means spin[k-1] == -1.
typedef struct {
    int16_t lit[3];
} XenoClause3;

// Pairwise Ising coupling J_ij * s_i * s_j (J > 0 ferromagnetic, J < 0 anti-ferromagnetic / XOR entanglement)
typedef struct {
    uint16_t var_i;
    uint16_t var_j;
    int16_t  weight_j; // Energy penalty when s_i * s_j == weight_j sign mismatch
} XenoCoupling2;

typedef struct {
    double   shannon_entropy_bits_per_byte;
    double   ones_ratio_percent;
    uint64_t total_page_collapses;
    uint64_t total_vacuum_decays;
    uint64_t total_anneal_race_flips;
    double   total_collapse_latency_us;
    uint32_t last_unsatisfied_clauses;
    uint64_t phys_footprint_bytes;
} XenoTelemetry;

// Lifecycle: Initializes persistent 8-core Super <-> Efficiency Silicon Race Bath & MMU SIGBUS/SIGSEGV trap
int xeno_init(size_t virtual_universe_bytes, int8_t **out_vacuum_ptr);
void xeno_shutdown(void);

// Bind a logical spin variable (0 .. XENO_MAX_VARS-1) to ANY byte offset inside the 64 GB virtual vacuum!
int xeno_bind_variable(uint16_t var_id, size_t byte_offset_in_vacuum);

// Impose laws of reality (constraints) on unallocated void WITHOUT allocating or computing anything
void xeno_clear_hamiltonian(void);
int xeno_add_3sat_clause(int16_t lit0, int16_t lit1, int16_t lit2);
int xeno_add_ising_coupling(uint16_t var_i, uint16_t var_j, int16_t j_coupling);

// Impose an external measurement/pinning on a variable; if it contradicts already-observed pages,
// XenoMem triggers "Vacuum Decay" (mprotect -> PROT_NONE), dissolving contradicted pages back into quantum foam!
int xeno_impose_measurement(uint16_t var_id, int8_t pinned_spin);

// Diagnostics & Silicon Race Bath entropy audit (verified ZERO PRNGs in binary!)
void xeno_measure_silicon_bath_entropy(double *out_shannon_bits, double *out_ones_pct);
XenoTelemetry xeno_get_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif
