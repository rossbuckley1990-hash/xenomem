#ifndef XENOMEM_H
#define XENOMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENO_MAX_VARS    4096
#define XENO_MAX_CLAUSES 32768

typedef struct {
    int16_t lit[3];
} XenoClause3;

typedef struct {
    uint16_t var_i;
    uint16_t var_j;
    int16_t  weight_j;
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

int xeno_init(size_t virtual_universe_bytes, int8_t **out_vacuum_ptr);
void xeno_shutdown(void);

int xeno_bind_variable(uint16_t var_id, size_t byte_offset_in_vacuum);

void xeno_clear_hamiltonian(void);
int xeno_add_3sat_clause(int16_t lit0, int16_t lit1, int16_t lit2);
int xeno_add_ising_coupling(uint16_t var_i, uint16_t var_j, int16_t j_coupling);

int xeno_impose_measurement(uint16_t var_id, int8_t pinned_spin);

void xeno_measure_silicon_bath_entropy(double *out_shannon_bits, double *out_ones_pct);
void xeno_measure_entropy_ex(
    int oscillators_active,
    double *out_shannon,
    double *out_min_entropy,
    double *out_markov_cond_entropy,
    double *out_lag1_corr
);

XenoTelemetry xeno_get_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif
