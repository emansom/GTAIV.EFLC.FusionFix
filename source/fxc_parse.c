/* fxc_parse.c - RAGE grcEffect (.fxc "rgxa") parser. See fxc_parse.h.
 * Portable C99, no D3D dependency. Compiles with gcc/clang and MSVC.
 *
 * VENDORED into the FusionFix ASI source tree from re/shader-precompile/ (W1).
 * Built as C (premake `source/**.c`); the ASI's C++ modules call it through the
 * extern "C" declarations in fxc_parse.h. Keep byte-identical to the W1 copy. */

#include "fxc_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* error string                                                        */
/* ------------------------------------------------------------------ */
static char g_err[512] = "ok";
const char *fxc_last_error(void) { return g_err; }
static void set_err(const char *fmt, const char *a, unsigned b) {
    /* tiny sprintf wrapper kept dependency-free */
    snprintf(g_err, sizeof(g_err), fmt, a ? a : "", b);
}

/* ------------------------------------------------------------------ */
/* db + growable owned-allocation registry                             */
/* ------------------------------------------------------------------ */
struct fxc_db {
    fxc_shader *uniques;   uint32_t unique_count, unique_cap;
    int32_t    *hslot;     uint32_t hcap;          /* open addressing -> unique idx, -1 empty */
    fxc_effect *effects;   uint32_t effect_count, effect_cap;
    fxc_stats   stats;
    void      **owned;     size_t owned_count, owned_cap;  /* all heap blocks to free */
};

static void *own(fxc_db *db, void *p) {
    if (!p) return NULL;
    if (db->owned_count == db->owned_cap) {
        size_t nc = db->owned_cap ? db->owned_cap * 2 : 64;
        void **no = (void **)realloc(db->owned, nc * sizeof(void *));
        if (!no) return p; /* leak-on-OOM is acceptable for this offline/launch tool */
        db->owned = no; db->owned_cap = nc;
    }
    db->owned[db->owned_count++] = p;
    return p;
}

static char *own_strdup(fxc_db *db, const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) return NULL;
    memcpy(d, s, n);
    return (char *)own(db, d);
}

/* ------------------------------------------------------------------ */
/* bounds-checked cursor                                               */
/* ------------------------------------------------------------------ */
typedef struct { const uint8_t *p; size_t n; size_t off; int bad; } cur;

static uint8_t  rd8(cur *c) {
    if (c->bad || c->off + 1 > c->n) { c->bad = 1; return 0; }
    return c->p[c->off++];
}
static uint16_t rd16(cur *c) {
    if (c->bad || c->off + 2 > c->n) { c->bad = 1; return 0; }
    uint16_t v = (uint16_t)(c->p[c->off] | (c->p[c->off + 1] << 8));
    c->off += 2; return v;
}
static uint32_t rd32(cur *c) {
    if (c->bad || c->off + 4 > c->n) { c->bad = 1; return 0; }
    uint32_t v = (uint32_t)c->p[c->off] | ((uint32_t)c->p[c->off + 1] << 8) |
                 ((uint32_t)c->p[c->off + 2] << 16) | ((uint32_t)c->p[c->off + 3] << 24);
    c->off += 4; return v;
}
static void skip(cur *c, size_t k) {
    if (c->bad || c->off + k > c->n) { c->bad = 1; return; }
    c->off += k;
}
/* STRING = u8 len + len bytes (len INCLUDES trailing NUL). Returns pointer into
 * the buffer (valid NUL-terminated C string), or NULL for an empty string. */
static const char *rdstr(cur *c) {
    uint8_t len = rd8(c);
    if (c->bad) return NULL;
    if (c->off + len > c->n) { c->bad = 1; return NULL; }
    const char *s = (len > 0) ? (const char *)(c->p + c->off) : "";
    c->off += len;
    return s;
}

/* ------------------------------------------------------------------ */
/* dedup                                                               */
/* ------------------------------------------------------------------ */
static uint64_t fnv1a64(const uint8_t *d, uint32_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

static void hash_rebuild(fxc_db *db, uint32_t newcap) {
    int32_t *ns = (int32_t *)malloc(newcap * sizeof(int32_t));
    if (!ns) return;
    for (uint32_t i = 0; i < newcap; i++) ns[i] = -1;
    for (uint32_t u = 0; u < db->unique_count; u++) {
        uint64_t h = db->uniques[u].hash;
        uint32_t m = newcap - 1, s = (uint32_t)h & m;
        while (ns[s] != -1) s = (s + 1) & m;
        ns[s] = (int32_t)u;
    }
    free(db->hslot);
    db->hslot = ns; db->hcap = newcap;
}

/* Intern a bytecode blob; returns its unique index. Increments ref_count. */
static uint32_t intern(fxc_db *db, fxc_stage stage, const uint8_t *bc, uint32_t size) {
    uint64_t h = fnv1a64(bc, size);
    if (db->unique_count * 2 >= db->hcap)
        hash_rebuild(db, db->hcap ? db->hcap * 2 : 4096);
    uint32_t m = db->hcap - 1, s = (uint32_t)h & m;
    while (db->hslot[s] != -1) {
        fxc_shader *u = &db->uniques[db->hslot[s]];
        if (u->hash == h && u->stage == stage && u->size == size &&
            memcmp(u->bytecode, bc, size) == 0) {
            u->ref_count++;
            return u->unique_index;
        }
        s = (s + 1) & m;
    }
    if (db->unique_count == db->unique_cap) {
        uint32_t nc = db->unique_cap ? db->unique_cap * 2 : 2048;
        fxc_shader *nu = (fxc_shader *)realloc(db->uniques, nc * sizeof(fxc_shader));
        if (!nu) return FXC_NO_SHADER;
        db->uniques = nu; db->unique_cap = nc;
    }
    uint32_t idx = db->unique_count++;
    fxc_shader *u = &db->uniques[idx];
    u->unique_index = idx; u->stage = stage; u->bytecode = bc;
    u->size = size; u->hash = h; u->ref_count = 1;
    db->hslot[s] = (int32_t)idx;
    return idx;
}

/* ------------------------------------------------------------------ */
/* fragment / variable / technique parsing                             */
/* ------------------------------------------------------------------ */

/* Parse one FRAGMENT header, intern its bytecode, return the unique index.
 * Records stage-appropriate token validation. */
static uint32_t parse_fragment(fxc_db *db, cur *c, fxc_stage stage,
                               const char *effect_name, int *token_ok) {
    uint8_t varCount = rd8(c);
    for (uint8_t i = 0; i < varCount; i++) {
        rd16(c);           /* type  */
        rd16(c);           /* index */
        rdstr(c);          /* name  */
        if (c->bad) return FXC_NO_SHADER;
    }
    uint16_t sz  = rd16(c);
    uint16_t sz2 = rd16(c);
    if (c->bad) return FXC_NO_SHADER;
    if (sz != sz2) {
        set_err("%s: shaderSize mismatch (%u)", effect_name, sz);
        c->bad = 1; return FXC_NO_SHADER;
    }
    if (c->off + sz > c->n || sz < 8) { c->bad = 1; return FXC_NO_SHADER; }
    const uint8_t *bc = c->p + c->off;
    /* validate version + end token */
    uint32_t ver = (uint32_t)bc[0] | ((uint32_t)bc[1] << 8) |
                   ((uint32_t)bc[2] << 16) | ((uint32_t)bc[3] << 24);
    uint32_t end = (uint32_t)bc[sz - 4] | ((uint32_t)bc[sz - 3] << 8) |
                   ((uint32_t)bc[sz - 2] << 16) | ((uint32_t)bc[sz - 1] << 24);
    uint32_t want = (stage == FXC_STAGE_VS) ? 0xFFFE0300u : 0xFFFF0300u;
    if (ver != want || end != 0x0000FFFFu) *token_ok = 0;
    c->off += sz;
    return intern(db, stage, bc, sz);
}

/* Skip one VARIABLE (global/local); we only need to walk past it to the
 * technique table. Format decoded & verified against RageShaderEditor XML. */
static void skip_variable(cur *c) {
    rd8(c);                 /* type      */
    rd8(c);                 /* arrayCount */
    rdstr(c);               /* name1 */
    rdstr(c);               /* name2 */
    uint8_t annCount = rd8(c);
    for (uint8_t i = 0; i < annCount && !c->bad; i++) {
        rdstr(c);                 /* annotation name */
        uint8_t atype = rd8(c);   /* value type: 0x01=Float, 0x02=String, else Int */
        if (atype == 0x02) rdstr(c);   /* String value: length-prefixed */
        else               skip(c, 4); /* Float/Int value: raw u32 */
    }
    uint8_t valCount = rd8(c);
    skip(c, (size_t)valCount * 4u);   /* each VALUE is a u32 */
}

/* ------------------------------------------------------------------ */
/* effect parse                                                        */
/* ------------------------------------------------------------------ */
static int parse_effect(fxc_db *db, const uint8_t *data, size_t size,
                        const char *name, const char *path) {
    cur cc; cc.p = data; cc.n = size; cc.off = 0; cc.bad = 0;
    cur *c = &cc;

    uint32_t magic = rd32(c);
    if (magic != 0x61786772u) {           /* "rgxa" */
        set_err("%s: bad magic 0x%08x", name, magic);
        return 0;
    }

    int token_ok = 1;

    uint8_t vsCount = rd8(c);
    uint32_t *vsmap = (uint32_t *)own(db, malloc((vsCount ? vsCount : 1) * sizeof(uint32_t)));
    for (uint8_t i = 0; i < vsCount; i++)
        vsmap[i] = parse_fragment(db, c, FXC_STAGE_VS, name, &token_ok);
    if (c->bad) { set_err("%s: VS fragment parse overran", name, 0); return 0; }

    uint8_t psStored = rd8(c);
    rd8(c);                    /* unk1 */
    rd32(c);                   /* unk2 */
    uint8_t psCount = (uint8_t)(psStored ? psStored - 1 : 0);
    uint32_t *psmap = (uint32_t *)own(db, malloc((psCount ? psCount : 1) * sizeof(uint32_t)));
    for (uint8_t i = 0; i < psCount; i++)
        psmap[i] = parse_fragment(db, c, FXC_STAGE_PS, name, &token_ok);
    if (c->bad) { set_err("%s: PS fragment parse overran", name, 0); return 0; }

    uint8_t gvc = rd8(c);
    for (uint8_t i = 0; i < gvc && !c->bad; i++) skip_variable(c);
    uint8_t lvc = rd8(c);
    for (uint8_t i = 0; i < lvc && !c->bad; i++) skip_variable(c);
    if (c->bad) { set_err("%s: variable table parse overran", name, 0); return 0; }

    uint8_t techCount = rd8(c);
    fxc_technique *techs = (fxc_technique *)own(db,
        calloc(techCount ? techCount : 1, sizeof(fxc_technique)));

    /* First pass over techniques to count passes/values for flat allocation. */
    size_t save = c->off;
    uint32_t totalPasses = 0, totalValues = 0;
    for (uint8_t t = 0; t < techCount && !c->bad; t++) {
        rdstr(c);                          /* name */
        uint8_t pc = rd8(c);
        for (uint8_t p = 0; p < pc && !c->bad; p++) {
            rd8(c); rd8(c);                /* vs, ps */
            uint8_t vc = rd8(c);
            totalValues += vc;
            skip(c, (size_t)vc * 8u);      /* {u32 type, u32 value} */
        }
        totalPasses += pc;
    }
    if (c->bad) { set_err("%s: technique table parse overran", name, 0); return 0; }

    /* Optional trailing per-fragment name table, present in a few effects
     * (e.g. gta_vehicle_licenseplate/track): (vsCount + psCount) consecutive
     * STRINGs "vs0".."vsN","ps0".."psN". Consume it to reach EOF if present. */
    if (!c->bad && c->off < size) {
        uint32_t names = (uint32_t)vsCount + psCount;
        for (uint32_t i = 0; i < names && !c->bad; i++) rdstr(c);
    }

    if (c->off != size) {
        /* Strong self-consistency check: the technique table is the last thing
         * in the file. A drift here means the variable walk was wrong. */
        set_err("%s: trailing %u bytes after techniques (parse drift)",
                name, (unsigned)(size - c->off));
        return 0;
    }

    fxc_pass       *passBlk = (fxc_pass *)own(db,
        calloc(totalPasses ? totalPasses : 1, sizeof(fxc_pass)));
    fxc_pass_value *valBlk  = (fxc_pass_value *)own(db,
        calloc(totalValues ? totalValues : 1, sizeof(fxc_pass_value)));

    /* Second pass: fill in. */
    c->off = save; c->bad = 0;
    uint32_t passCursor = 0, valCursor = 0;
    for (uint8_t t = 0; t < techCount; t++) {
        const char *tname = rdstr(c);
        uint8_t pc = rd8(c);
        techs[t].name = tname ? tname : "";
        techs[t].passes = passBlk + passCursor;
        techs[t].pass_count = pc;
        for (uint8_t p = 0; p < pc; p++) {
            uint8_t vs = rd8(c);
            uint8_t ps = rd8(c);           /* stored +1, 0 = none */
            uint8_t vc = rd8(c);
            fxc_pass *pass = &passBlk[passCursor++];
            pass->vs_local = vs;
            pass->ps_local = ps ? (int32_t)(ps - 1) : -1;
            pass->vs_unique = (vs < vsCount) ? vsmap[vs] : FXC_NO_SHADER;
            pass->ps_unique = (pass->ps_local >= 0 && pass->ps_local < (int32_t)psCount)
                              ? psmap[pass->ps_local] : FXC_NO_SHADER;
            pass->values = valBlk + valCursor;
            pass->value_count = vc;
            for (uint8_t v = 0; v < vc; v++) {
                valBlk[valCursor].type  = rd32(c);
                valBlk[valCursor].value = rd32(c);
                valCursor++;
            }
        }
    }
    if (c->bad) { set_err("%s: technique fill overran", name, 0); return 0; }

    /* Commit the effect. */
    if (db->effect_count == db->effect_cap) {
        uint32_t nc = db->effect_cap ? db->effect_cap * 2 : 128;
        fxc_effect *ne = (fxc_effect *)realloc(db->effects, nc * sizeof(fxc_effect));
        if (!ne) { set_err("OOM growing effects", name, 0); return 0; }
        db->effects = ne; db->effect_cap = nc;
    }
    fxc_effect *ef = &db->effects[db->effect_count++];
    ef->name = own_strdup(db, name);
    ef->path = own_strdup(db, path ? path : name);
    ef->vs_count = vsCount;
    ef->ps_count = psCount;
    ef->vs_local_to_unique = vsmap;
    ef->ps_local_to_unique = psmap;
    ef->techniques = techs;
    ef->technique_count = techCount;

    /* stats */
    db->stats.total_vs_blobs += vsCount;
    db->stats.total_ps_blobs += psCount;
    db->stats.technique_count += techCount;
    db->stats.pass_count += totalPasses;

    if (!token_ok)
        set_err("%s: WARNING token validation failed on a fragment", name, 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* file / dir loading                                                  */
/* ------------------------------------------------------------------ */
static fxc_db *db_new(void) {
    fxc_db *db = (fxc_db *)calloc(1, sizeof(fxc_db));
    return db;
}

static void finalize_stats(fxc_db *db) {
    db->stats.effect_count = db->effect_count;
    db->stats.total_blobs = db->stats.total_vs_blobs + db->stats.total_ps_blobs;
    uint32_t uv = 0, up = 0;
    for (uint32_t i = 0; i < db->unique_count; i++)
        (db->uniques[i].stage == FXC_STAGE_VS) ? uv++ : up++;
    db->stats.unique_vs = uv;
    db->stats.unique_ps = up;
    db->stats.unique_total = db->unique_count;
}

/* read whole file into a db-owned buffer */
static uint8_t *read_file(fxc_db *db, const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_err("cannot open %s", path, 0); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); set_err("empty/bad file %s", path, 0); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); set_err("OOM reading %s", path, 0); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); set_err("short read %s", path, 0); return NULL; }
    own(db, buf);
    *out_size = (size_t)sz;
    return buf;
}

static const char *basename_noext(const char *path, char *tmp, size_t tmpsz) {
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    size_t n = 0;
    while (b[n] && b[n] != '.' && n + 1 < tmpsz) { tmp[n] = b[n]; n++; }
    tmp[n] = 0;
    return tmp;
}

fxc_db *fxc_load_file(const char *path) {
    fxc_db *db = db_new();
    if (!db) { set_err("OOM", NULL, 0); return NULL; }
    size_t sz = 0;
    uint8_t *buf = read_file(db, path, &sz);
    if (!buf) { fxc_free(db); return NULL; }
    char nb[256];
    basename_noext(path, nb, sizeof(nb));
    if (!parse_effect(db, buf, sz, nb, path)) db->stats.parse_errors++;
    finalize_stats(db);
    return db;
}

fxc_db *fxc_parse_buffer(const void *data, size_t size, const char *name) {
    fxc_db *db = db_new();
    if (!db) { set_err("OOM", NULL, 0); return NULL; }
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf) { fxc_free(db); set_err("OOM", NULL, 0); return NULL; }
    memcpy(buf, data, size);
    own(db, buf);
    if (!parse_effect(db, buf, size, name ? name : "(buffer)", name))
        db->stats.parse_errors++;
    finalize_stats(db);
    return db;
}

/* portable strdup (avoids POSIX/MSVC-only strdup under strict -std=c99) */
static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- directory enumeration (platform-specific) ------------------- */
#if defined(_WIN32)
#include <windows.h>
static int list_fxc(const char *dir, char ***out, uint32_t *n) {
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*.fxc", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char **list = NULL; uint32_t cnt = 0, cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 128; list = (char **)realloc(list, cap * sizeof(char *)); }
        char full[1024];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        list[cnt++] = dup_cstr(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    *out = list; *n = cnt;
    return 1;
}
#else
#include <dirent.h>
static int has_fxc_ext(const char *nm) {
    size_t n = strlen(nm);
    return n > 4 && strcmp(nm + n - 4, ".fxc") == 0;
}
static int list_fxc(const char *dir, char ***out, uint32_t *n) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char **list = NULL; uint32_t cnt = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_fxc_ext(e->d_name)) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 128; list = (char **)realloc(list, cap * sizeof(char *)); }
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        list[cnt++] = dup_cstr(full);
    }
    closedir(d);
    *out = list; *n = cnt;
    return 1;
}
#endif

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

fxc_db *fxc_load_all(const char *dir) {
    char **files = NULL; uint32_t nf = 0;
    if (!list_fxc(dir, &files, &nf)) { set_err("cannot open dir %s", dir, 0); return NULL; }
    qsort(files, nf, sizeof(char *), cmp_str);   /* deterministic order */

    fxc_db *db = db_new();
    if (!db) { set_err("OOM", NULL, 0); return NULL; }

    for (uint32_t i = 0; i < nf; i++) {
        size_t sz = 0;
        uint8_t *buf = read_file(db, files[i], &sz);
        char nb[256];
        basename_noext(files[i], nb, sizeof(nb));
        if (!buf || !parse_effect(db, buf, sz, nb, files[i])) {
            db->stats.parse_errors++;
            fprintf(stderr, "[fxc_parse] parse error: %s\n", fxc_last_error());
        }
        free(files[i]);
    }
    free(files);
    finalize_stats(db);
    return db;
}

/* ------------------------------------------------------------------ */
/* accessors + free                                                    */
/* ------------------------------------------------------------------ */
uint32_t fxc_unique_count(const fxc_db *db) { return db ? db->unique_count : 0; }
const fxc_shader *fxc_unique_shader(const fxc_db *db, uint32_t i) {
    if (!db || i >= db->unique_count) return NULL;
    return &db->uniques[i];
}
uint32_t fxc_effect_count(const fxc_db *db) { return db ? db->effect_count : 0; }
const fxc_effect *fxc_get_effect(const fxc_db *db, uint32_t i) {
    if (!db || i >= db->effect_count) return NULL;
    return &db->effects[i];
}
void fxc_get_stats(const fxc_db *db, fxc_stats *out) {
    if (out && db) *out = db->stats;
}

void fxc_free(fxc_db *db) {
    if (!db) return;
    for (size_t i = 0; i < db->owned_count; i++) free(db->owned[i]);
    free(db->owned);
    free(db->uniques);
    free(db->hslot);
    free(db->effects);
    free(db);
}
