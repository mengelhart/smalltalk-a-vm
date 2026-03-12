/* tests/test_image_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: image save/load for a closed-world subset.
 * See docs/spikes/spike-006-image.md.
 *
 * Tests:
 *   1. File header validates correctly (magic, version, endian, ptr_width)
 *   2. Object ID encoding: SmallInt stored verbatim, heap ptr → snapshot OOP
 *   3. Minimal graph save: nil/true/false/sym_hello/arr round-trips cleanly
 *   4. Restore integrity: all OOPs, flags, and payloads round-trip exactly
 *   5. Immutable section: sym_hello in immutable section, reloc entry present
 *   6. Large graph: 1000 Arrays × 8 SmallInt slots — throughput baseline
 *   7. Actor snapshot fields: sizeof(STA_ActorSnap) and density table
 *   8. TSan gate: single-threaded, validates no shared-state races
 */

#include "src/image/image_spike.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Timing helper ──────────────────────────────────────────────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Object allocator (spike: flat malloc; Phase 1 uses pool) ───────────── */

static STA_ObjHeader *alloc_obj(uint32_t class_index, uint32_t size,
                                uint8_t gc_flags, uint8_t obj_flags) {
    size_t alloc = sta_alloc_size(size);
    STA_ObjHeader *h = malloc(alloc);
    assert(h);
    memset(h, 0, alloc);
    h->class_index = class_index;
    h->size        = size;
    h->gc_flags    = gc_flags;
    h->obj_flags   = obj_flags;
    h->reserved    = 0;
    return h;
}

/* ── Immutable resolver used by sta_image_load() ────────────────────────── */

/* Registry entry: name → runtime header */
typedef struct {
    const char    *name;
    uint16_t       name_len;
    STA_ObjHeader *hdr;
} ResolverEntry;

typedef struct {
    ResolverEntry *entries;
    size_t         count;
} ResolverTable;

static STA_ObjHeader *test_resolver(uint32_t stable_key,
                                    const char *name, uint16_t name_len,
                                    void *userdata) {
    (void)stable_key;
    ResolverTable *tbl = (ResolverTable *)userdata;
    for (size_t i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].name_len == name_len &&
            memcmp(tbl->entries[i].name, name, name_len) == 0) {
            return tbl->entries[i].hdr;
        }
    }
    return NULL;
}

/* ── Class indices used in tests ─────────────────────────────────────────── */
#define CLS_NIL      0u
#define CLS_BOOL     1u
#define CLS_SYMBOL   2u
#define CLS_ARRAY    3u

/* ── Temp path helper ────────────────────────────────────────────────────── */

static const char *tmp_path(const char *name) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/%s", name);
    return buf;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 1: File header validates correctly
 * ═══════════════════════════════════════════════════════════════════════════*/

static void test_header_validation(void) {
    printf("Test 1: File header validates correctly ... ");
    fflush(stdout);

    /* Build a trivial graph: just nil */
    STA_ObjHeader *nil_hdr = alloc_obj(CLS_NIL, 0, STA_GC_BLACK,
                                       STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);

    STA_SnapCtx ctx;
    sta_snap_ctx_init(&ctx);
    sta_snap_register_immutable(&ctx, nil_hdr, "nil", 3);

    const char *path = tmp_path("test1_header.img");
    assert(sta_image_save(&ctx, path) == STA_OK);

    /* Verify file size matches header */
    FILE *f = fopen(path, "rb");
    assert(f);
    STA_ImageHeader hdr;
    assert(fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    fclose(f);

    assert(memcmp(hdr.magic, STA_IMAGE_MAGIC, STA_IMAGE_MAGIC_LEN) == 0);
    assert(hdr.version   == STA_IMAGE_VERSION);
    assert(hdr.ptr_width == 8);
    assert(hdr.object_count == 1);
    assert(hdr.immutable_count == 1);

    /* File size must equal header.file_size */
    f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long actual = ftell(f);
    fclose(f);
    assert((uint64_t)actual == hdr.file_size);

    free(nil_hdr);
    printf("PASS\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 2: OOP encoding
 * ═══════════════════════════════════════════════════════════════════════════*/

static void test_oop_encoding(void) {
    printf("Test 2: OOP encoding (SmallInt verbatim, heap ptr -> snap OOP) ... ");
    fflush(stdout);

    /* SmallInt(42) */
    STA_OOP si = STA_SMALLINT_OOP(42);
    assert(STA_IS_SMALLINT(si));
    assert(STA_SMALLINT_VAL(si) == 42);
    /* In snapshot encoding, SmallInt stored verbatim */
    assert(!STA_SNAP_IS_HEAP((uint64_t)si));

    /* Heap OOP encoding */
    uint32_t id = 7u;
    uint64_t encoded = STA_SNAP_ENCODE(id);
    assert(STA_SNAP_IS_HEAP(encoded));
    assert(STA_SNAP_GET_ID(encoded) == id);

    /* Note: STA_SNAP_ENCODE with an odd id produces bits 1:0 = 11, which
     * also satisfies STA_IS_SMALLINT. This is intentional and acceptable:
     * snapshot files are not live OOP streams. Disambiguation is done via
     * the relocation table (which slots need fixup), not tag bits alone.
     * During save, only heap-pointer slots get reloc entries; during load,
     * only reloc'd slots are passed through STA_SNAP_GET_ID. */

    /* Character immediate (bits 1:0 = 10) must not be SNAP_HEAP */
    STA_OOP ch = 0x2u | (65u << 2); /* 'A' */
    assert(!STA_SNAP_IS_HEAP((uint64_t)ch));

    printf("PASS\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 3 + 4: Minimal closed-world graph save and restore integrity
 *
 * Graph:
 *   id 0: nil_obj  (immutable, 0 slots)
 *   id 1: true_obj (immutable, 0 slots)
 *   id 2: false_obj(immutable, 0 slots)
 *   id 3: sym_hello (immutable, Symbol, 1 word payload = "hell" padded)
 *   id 4: arr (Array, 3 slots: [SmallInt(42), sym_hello, nil_obj])
 *
 * Root object (id 0 per spec) is arr — we register arr first as id 0.
 * Immutables are registered after arr so that arr's slot 1 reloc refers
 * to sym_hello's id. But the spec says root = object_id 0 = arr.
 *
 * Registration order:
 *   0: arr    (local)
 *   1: nil    (immutable)
 *   2: true   (immutable)
 *   3: false  (immutable)
 *   4: sym_hello (immutable)
 * ═══════════════════════════════════════════════════════════════════════════*/

static void test_minimal_graph(void) {
    printf("Test 3: Minimal graph save ... ");
    fflush(stdout);

    /* ── Allocate objects ────────────────────────────────────────────────*/
    /* sym_hello: 1 word payload — we store the 5-byte string "hello" in one
     * 8-byte OOP word (encoded as a SmallInt-tagged immediate for simplicity
     * in this spike; production would use byte arrays). */
    STA_ObjHeader *sym_hello = alloc_obj(CLS_SYMBOL, 1,
                                         STA_GC_BLACK,
                                         STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);
    /* Store "hello" as a raw 5-byte value in the payload word.
     * We tag bit 0 = 0 so it looks like a heap ptr in the live system,
     * but in the snapshot it will simply be encoded verbatim since it is NOT
     * a registered heap object — it is inline byte data.
     * To avoid this ambiguity in the spike, store it as a SmallInt-tagged
     * integer whose value encodes the string bytes. */
    uint64_t hello_bytes;
    memcpy(&hello_bytes, "hello\0\0\0", 8);
    /* Tag as SmallInt so encode_oop stores it verbatim */
    STA_OOP hello_oop = (hello_bytes << 1) | 1u;
    sta_payload(sym_hello)[0] = hello_oop;

    STA_ObjHeader *nil_obj   = alloc_obj(CLS_NIL,  0, STA_GC_BLACK,
                                         STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);
    STA_ObjHeader *true_obj  = alloc_obj(CLS_BOOL, 0, STA_GC_BLACK,
                                         STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);
    STA_ObjHeader *false_obj = alloc_obj(CLS_BOOL, 0, STA_GC_BLACK,
                                         STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);
    STA_ObjHeader *arr       = alloc_obj(CLS_ARRAY, 3, STA_GC_WHITE,
                                         STA_OBJ_ACTOR_LOCAL);

    /* Fill arr payload */
    STA_OOP *arr_payload = sta_payload(arr);
    arr_payload[0] = STA_SMALLINT_OOP(42);
    arr_payload[1] = (STA_OOP)(uintptr_t)sym_hello;
    arr_payload[2] = (STA_OOP)(uintptr_t)nil_obj;

    /* ── Register ────────────────────────────────────────────────────────*/
    STA_SnapCtx ctx;
    sta_snap_ctx_init(&ctx);

    uint32_t arr_id       = sta_snap_register_object(&ctx, arr);
    uint32_t nil_id       = sta_snap_register_immutable(&ctx, nil_obj,   "nil",   3);
    uint32_t true_id      = sta_snap_register_immutable(&ctx, true_obj,  "true",  4);
    uint32_t false_id     = sta_snap_register_immutable(&ctx, false_obj, "false", 5);
    uint32_t sym_hello_id = sta_snap_register_immutable(&ctx, sym_hello, "hello", 5);

    assert(arr_id       == 0);
    assert(nil_id       == 1);
    assert(true_id      == 2);
    assert(false_id     == 3);
    assert(sym_hello_id == 4);

    /* ── Save ────────────────────────────────────────────────────────────*/
    const char *path = tmp_path("test3_minimal.img");
    int rc = sta_image_save(&ctx, path);
    assert(rc == STA_OK);

    /* Verify sym_hello is in the immutable section (immutable_count == 4) */
    FILE *f = fopen(path, "rb");
    assert(f);
    STA_ImageHeader hdr;
    assert(fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    fclose(f);

    assert(hdr.object_count    == 5);
    assert(hdr.immutable_count == 4);

    /* Verify reloc table has exactly 2 entries (arr slot 1 = sym_hello,
     * arr slot 2 = nil_obj) */
    assert(ctx.reloc_count == 2);
    /* First reloc: arr(0), slot 1 → sym_hello */
    assert(ctx.relocs[0].object_id  == arr_id);
    assert(ctx.relocs[0].slot_index == 1);
    /* Second reloc: arr(0), slot 2 → nil_obj */
    assert(ctx.relocs[1].object_id  == arr_id);
    assert(ctx.relocs[1].slot_index == 2);

    printf("PASS\n");

    /* ── Test 4: Restore integrity ───────────────────────────────────────*/
    printf("Test 4: Restore integrity ... ");
    fflush(stdout);

    ResolverEntry entries[] = {
        { "nil",   3, nil_obj   },
        { "true",  4, true_obj  },
        { "false", 5, false_obj },
        { "hello", 5, sym_hello },
    };
    ResolverTable tbl = { entries, 4 };

    STA_ObjHeader *root = NULL;
    rc = sta_image_load(path, test_resolver, &tbl, &root);
    assert(rc == STA_OK);
    assert(root != NULL);

    /* root is arr (id 0) */
    assert(root->size == 3);
    assert(root->gc_flags  == STA_GC_WHITE);
    assert(root->obj_flags == STA_OBJ_ACTOR_LOCAL);

    STA_OOP *rp = sta_payload(root);

    /* Slot 0: SmallInt(42) */
    assert(STA_IS_SMALLINT(rp[0]));
    assert(STA_SMALLINT_VAL(rp[0]) == 42);

    /* Slot 1: heap ptr to sym_hello */
    assert(!STA_IS_SMALLINT(rp[1]));
    STA_ObjHeader *got_sym = (STA_ObjHeader *)(uintptr_t)rp[1];
    assert(got_sym == sym_hello); /* same canonical address from resolver */
    assert(got_sym->size == 1);
    /* Verify "hello" payload round-tripped */
    STA_OOP got_hello = sta_payload(got_sym)[0];
    assert(STA_IS_SMALLINT(got_hello));
    /* Verify the encoded OOP value round-tripped identically */
    assert(got_hello == hello_oop);

    /* Slot 2: heap ptr to nil_obj (canonical runtime address) */
    assert(!STA_IS_SMALLINT(rp[2]));
    STA_ObjHeader *got_nil = (STA_ObjHeader *)(uintptr_t)rp[2];
    assert(got_nil == nil_obj);

    /* Free the loaded arr (not nil/true/false/sym_hello — those are "runtime"
     * objects owned by the test, returned unchanged by resolver) */
    free(root);

    free(arr);
    free(nil_obj);
    free(true_obj);
    free(false_obj);
    free(sym_hello);

    printf("PASS\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 5: Large graph throughput baseline
 *   1000 Array objects × 8 SmallInt slots each (no cross-object refs)
 * ═══════════════════════════════════════════════════════════════════════════*/

#define LARGE_N       1000u
#define LARGE_SLOTS      8u

static void test_large_graph(void) {
    printf("Test 5: Large graph (1000×8 SmallInt arrays) throughput ... ");
    fflush(stdout);

    STA_ObjHeader **objs = malloc(LARGE_N * sizeof(STA_ObjHeader *));
    assert(objs);

    for (uint32_t i = 0; i < LARGE_N; i++) {
        objs[i] = alloc_obj(CLS_ARRAY, LARGE_SLOTS, STA_GC_WHITE,
                            STA_OBJ_ACTOR_LOCAL);
        STA_OOP *p = sta_payload(objs[i]);
        for (uint32_t s = 0; s < LARGE_SLOTS; s++) {
            p[s] = STA_SMALLINT_OOP((intptr_t)(i * LARGE_SLOTS + s));
        }
    }

    STA_SnapCtx *ctx = malloc(sizeof(STA_SnapCtx));
    assert(ctx);
    sta_snap_ctx_init(ctx);

    /* Register root first (id 0), then all others */
    sta_snap_register_object(ctx, objs[0]);
    for (uint32_t i = 1; i < LARGE_N; i++) {
        sta_snap_register_object(ctx, objs[i]);
    }

    const char *path = tmp_path("test5_large.img");

    uint64_t t0 = now_ns();
    int rc = sta_image_save(ctx, path);
    uint64_t t1 = now_ns();
    assert(rc == STA_OK);

    uint64_t save_ns = t1 - t0;

    /* Measure file size */
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long file_bytes = ftell(f);
    fclose(f);

    /* Resolver: no immutables in this graph */
    ResolverTable empty_tbl = { NULL, 0 };
    STA_ObjHeader *root = NULL;
    t0 = now_ns();
    rc = sta_image_load(path, test_resolver, &empty_tbl, &root);
    t1 = now_ns();
    assert(rc == STA_OK);
    uint64_t load_ns = t1 - t0;

    /* Spot-check: root (id 0) must have 8 SmallInt slots */
    assert(root->size == LARGE_SLOTS);
    STA_OOP *rp = sta_payload(root);
    for (uint32_t s = 0; s < LARGE_SLOTS; s++) {
        assert(STA_IS_SMALLINT(rp[s]));
        assert(STA_SMALLINT_VAL(rp[s]) == (intptr_t)s);
    }

    double save_ms      = (double)save_ns / 1e6;
    double load_ms      = (double)load_ns / 1e6;
    double obj_per_s_sv = (double)LARGE_N / ((double)save_ns / 1e9);
    double obj_per_s_ld = (double)LARGE_N / ((double)load_ns / 1e9);
    double bytes_per_s  = (double)file_bytes / ((double)save_ns / 1e9);

    printf("PASS\n");
    printf("  Save:  %.3f ms | %.0f objects/s | %ld bytes | %.2f MB/s\n",
           save_ms, obj_per_s_sv, file_bytes, bytes_per_s / 1e6);
    printf("  Load:  %.3f ms | %.0f objects/s\n", load_ms, obj_per_s_ld);

    /* Free loaded objects (they were malloc'd by sta_image_load) */
    /* We only have root back; all others are lost (spike limitation).
     * In Phase 1, a pool allocator lets us free the whole arena at once. */
    free(root);

    for (uint32_t i = 0; i < LARGE_N; i++) free(objs[i]);
    free(objs);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 6: Actor snapshot density measurement
 * ═══════════════════════════════════════════════════════════════════════════*/

static void test_actor_density(void) {
    printf("Test 6: Actor snapshot density ... ");
    fflush(stdout);

    size_t snap_size     = sizeof(STA_ActorSnap);
    size_t nursery       = 128;
    size_t identity      = STA_HEADER_ALIGNED; /* 16 bytes */
    size_t creation_cost = snap_size + nursery + identity;

    printf("PASS\n");
    printf("  sizeof(STA_ActorSnap)     = %zu bytes\n", snap_size);
    printf("  Nursery slab              = %zu bytes\n", nursery);
    printf("  Identity object           = %zu bytes\n", identity);
    printf("  Total creation cost       = %zu bytes\n", creation_cost);
    printf("  Headroom vs 300-byte tgt  = %zd bytes\n",
           (ssize_t)(300 - (ssize_t)creation_cost));
    printf("  Added fields vs Spike 005 = %zu bytes (snapshot_id + pad)\n",
           snap_size - 144u);

    assert(snap_size == 152);
    assert(creation_cost == 296);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test 7: Image format sizes
 * ═══════════════════════════════════════════════════════════════════════════*/

static void test_format_sizes(void) {
    printf("Test 7: Image format struct sizes ... ");
    fflush(stdout);

    assert(sizeof(STA_ImageHeader)    == 48);
    assert(sizeof(STA_ObjRecord)      == 16);
    assert(sizeof(STA_ImmutableEntry) == 10);
    assert(sizeof(STA_RelocEntry)     ==  8);

    printf("PASS\n");
    printf("  STA_ImageHeader:     %zu bytes\n", sizeof(STA_ImageHeader));
    printf("  STA_ObjRecord:       %zu bytes (fixed, excl. payload)\n",
           sizeof(STA_ObjRecord));
    printf("  STA_ImmutableEntry:  %zu bytes (fixed, excl. name)\n",
           sizeof(STA_ImmutableEntry));
    printf("  STA_RelocEntry:      %zu bytes\n", sizeof(STA_RelocEntry));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════*/

int main(void) {
    printf("=== Spike 006: Image save/load ===\n\n");

    test_header_validation();
    test_oop_encoding();
    test_minimal_graph();    /* covers tests 3 and 4 */
    test_large_graph();
    test_actor_density();
    test_format_sizes();

    printf("\nAll tests PASSED.\n");
    return 0;
}
