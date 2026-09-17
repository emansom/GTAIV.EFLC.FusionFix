/* fxc_parse.h - RAGE grcEffect (.fxc, magic "rgxa") static parser / extractor.
 *
 * Work item W1 of the GTA IV launch-time shader-precompilation feature.
 * Enumerates every embedded SM3.0 vertex/pixel shader blob from the game's
 * compiled effect containers, deduplicates by bytecode, and exposes the
 * technique/pass pairings + per-pass render-state hints (PASS_VALUE) that the
 * runtime precompiler (W3) keys its dummy draws on.
 *
 * PORTABLE: pure byte parsing, no OS / D3D dependency. Compiles as C99 or C++
 * with gcc/clang (native Linux verification build) AND MSVC (msvc-wine, ASI).
 *
 * VENDORED into the FusionFix ASI source tree from re/shader-precompile/ (W1).
 * Keep in sync with that authoritative copy; do not diverge the parser.
 *
 * ------------------------------------------------------------------------
 * Typical runtime use (Wave-2 precompiler, W3), pseudo-code:
 *
 *     fxc_db *db = fxc_load_all(shader_dir);          // parse all 107 .fxc
 *     uint32_t n = fxc_unique_count(db);
 *     IDirect3DVertexShader9 **vs = calloc(n, ...);   // handle table, indexed
 *     IDirect3DPixelShader9  **ps = calloc(n, ...);   //   by unique_index
 *     for (uint32_t i = 0; i < n; i++) {
 *         const fxc_shader *s = fxc_unique_shader(db, i);
 *         if (s->stage == FXC_STAGE_VS)
 *             dev->CreateVertexShader((const DWORD*)s->bytecode, &vs[i]);
 *         else
 *             dev->CreatePixelShader ((const DWORD*)s->bytecode, &ps[i]);
 *     }
 *     for (uint32_t e = 0; e < fxc_effect_count(db); e++) {
 *         const fxc_effect *ef = fxc_get_effect(db, e);
 *         for (uint32_t t = 0; t < ef->technique_count; t++)
 *             for (uint32_t p = 0; p < ef->techniques[t].pass_count; p++) {
 *                 const fxc_pass *pass = &ef->techniques[t].passes[p];
 *                 dev->SetVertexShader(vs[pass->vs_unique]);
 *                 if (pass->ps_unique != FXC_NO_SHADER)
 *                     dev->SetPixelShader(ps[pass->ps_unique]);
 *                 apply_pass_render_state(dev, pass->values, pass->value_count);
 *                 issue_degenerate_dummy_draw(dev);   // warms the pipeline
 *             }
 *     }
 *     fxc_free(db);
 *
 * Notes for the consumer:
 *  - bytecode is a raw D3D9 SM3.0 token stream, u32-aligned, verbatim from the
 *    container (starts vs_3_0=0xFFFE0300 / ps_3_0=0xFFFF0300, ends 0x0000FFFF),
 *    directly acceptable by Create{Vertex,Pixel}Shader. size is in BYTES.
 *  - The unique table interleaves VS and PS; each entry carries its stage, so a
 *    single handle array indexed by unique_index works if you branch on stage
 *    (or keep two arrays as above and index both by unique_index).
 *  - Pointers returned (bytecode, names, arrays) are owned by the fxc_db and
 *    stay valid until fxc_free(). Do not free them yourself.
 * ------------------------------------------------------------------------
 */
#ifndef FXC_PARSE_H
#define FXC_PARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel: a pass with no pixel shader (ps stored as 0 in the container). */
#define FXC_NO_SHADER 0xFFFFFFFFu

typedef enum {
    FXC_STAGE_VS = 0,   /* vertex shader   (vs_3_0, version token 0xFFFE0300) */
    FXC_STAGE_PS = 1    /* pixel  shader   (ps_3_0, version token 0xFFFF0300) */
} fxc_stage;

/* One unique (deduplicated) shader in the global unique table. */
typedef struct {
    uint32_t       unique_index; /* == its index in the unique table (0..count-1) */
    fxc_stage      stage;        /* VS or PS */
    const uint8_t *bytecode;     /* raw SM3.0 token stream (db-owned) */
    uint32_t       size;         /* bytecode length in bytes */
    uint64_t       hash;         /* FNV-1a-64 of the bytecode (bucketing/debug) */
    uint32_t       ref_count;    /* # of fragment slots sharing this exact bytecode */
} fxc_shader;

/* One render-state key/value the pass programs (the container's PASS_VALUE pairs).
 * Captured verbatim (raw ints); interpretation is left to the precompiler / W6. */
typedef struct {
    uint32_t type;   /* render-state key   (RAGE grcEffect state id) */
    uint32_t value;  /* render-state value */
} fxc_pass_value;

/* One pass = a (VS, PS) pairing plus its render-state hints. */
typedef struct {
    int32_t               vs_local;    /* raw VS fragment index within the effect */
    int32_t               ps_local;    /* raw PS fragment index within the effect; -1 if none */
    uint32_t              vs_unique;   /* index into the unique table for this VS */
    uint32_t              ps_unique;   /* index into the unique table for this PS, or FXC_NO_SHADER */
    const fxc_pass_value *values;      /* [value_count] PASS_VALUE pairs (db-owned) */
    uint32_t              value_count;
} fxc_pass;

typedef struct {
    const char     *name;        /* technique name (db-owned C string) */
    const fxc_pass *passes;      /* [pass_count] (db-owned) */
    uint32_t        pass_count;
} fxc_technique;

typedef struct {
    const char          *name;               /* effect basename w/o ".fxc" */
    const char          *path;               /* source file path (or synthetic) */
    uint32_t             vs_count;           /* embedded VS fragments in this effect */
    uint32_t             ps_count;           /* embedded PS fragments in this effect */
    const uint32_t      *vs_local_to_unique; /* [vs_count] local VS idx -> unique idx */
    const uint32_t      *ps_local_to_unique; /* [ps_count] local PS idx -> unique idx */
    const fxc_technique *techniques;         /* [technique_count] (db-owned) */
    uint32_t             technique_count;
} fxc_effect;

/* Aggregate statistics across everything loaded into the db. */
typedef struct {
    uint32_t effect_count;    /* .fxc files parsed OK */
    uint32_t total_vs_blobs;  /* VS fragments across all effects (pre-dedup) */
    uint32_t total_ps_blobs;  /* PS fragments across all effects (pre-dedup) */
    uint32_t total_blobs;     /* total_vs_blobs + total_ps_blobs */
    uint32_t unique_vs;       /* unique VS bytecodes */
    uint32_t unique_ps;       /* unique PS bytecodes */
    uint32_t unique_total;    /* unique_vs + unique_ps */
    uint32_t technique_count; /* techniques across all effects */
    uint32_t pass_count;      /* passes across all effects */
    uint32_t parse_errors;    /* files that failed to parse cleanly */
} fxc_stats;

typedef struct fxc_db fxc_db; /* opaque */

/* Parse every "*.fxc" in `dir`. Returns a db (possibly with parse_errors>0), or
 * NULL only on a hard failure (dir unreadable / OOM). Check fxc_last_error(). */
fxc_db *fxc_load_all(const char *dir);

/* Parse a single .fxc file into a one-effect db. NULL on failure. */
fxc_db *fxc_load_file(const char *path);

/* Parse a single in-memory .fxc image (the bytes are copied). `name` labels the
 * effect (may be NULL). NULL on failure. */
fxc_db *fxc_parse_buffer(const void *data, size_t size, const char *name);

/* Release everything (buffers, tables, strings). Safe on NULL. */
void fxc_free(fxc_db *db);

/* Unique deduplicated shader table. */
uint32_t          fxc_unique_count(const fxc_db *db);
const fxc_shader *fxc_unique_shader(const fxc_db *db, uint32_t unique_index);

/* Effects / techniques / passes. */
uint32_t          fxc_effect_count(const fxc_db *db);
const fxc_effect *fxc_get_effect(const fxc_db *db, uint32_t effect_index);

/* Aggregate stats (safe with out==NULL as a no-op). */
void fxc_get_stats(const fxc_db *db, fxc_stats *out);

/* Human-readable last error/warning (static buffer; overwritten by each call
 * that fails). Never NULL. */
const char *fxc_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FXC_PARSE_H */
