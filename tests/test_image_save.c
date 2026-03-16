/* tests/test_image_save.c
 * Smoke test: bootstrap + kernel load → image save → verify file structure.
 * Phase 1, Epic 10.
 */
#include <sta/vm.h>
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "image/image.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_ImmutableSpace *imm;
static STA_SymbolTable    *syms;
static STA_Heap           *heap;
static STA_ClassTable     *ct;
static const char         *image_path = "/tmp/sta_test_image_save.stai";

static void setup(void) {
    heap = sta_heap_create(4 * 1024 * 1024);
    imm  = sta_immutable_space_create(4 * 1024 * 1024);
    syms = sta_symbol_table_create(512);
    ct   = sta_class_table_create();

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);

    int rc = sta_kernel_load_all(KERNEL_DIR);
    assert(rc == 0);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_save_returns_ok(void) {
    int rc = sta_image_save_to_file(image_path, heap, imm, syms, ct);
    assert(rc == STA_OK);
}

static void test_file_exists_and_has_size(void) {
    FILE *f = fopen(image_path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    assert(size > 48);  /* at minimum the header */
    fclose(f);
}

static void test_header_magic(void) {
    FILE *f = fopen(image_path, "rb");
    assert(f);
    uint8_t magic[4];
    assert(fread(magic, 1, 4, f) == 4);
    assert(memcmp(magic, STA_IMAGE_MAGIC, 4) == 0);
    fclose(f);
}

static void test_header_fields(void) {
    FILE *f = fopen(image_path, "rb");
    assert(f);
    STA_ImageHeader hdr;
    assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
    fclose(f);

    /* Version. */
    assert(hdr.version == STA_IMAGE_VERSION);

    /* Endian matches host. */
    uint16_t v = 1;
    uint8_t expected_endian = (*(uint8_t *)&v == 1)
        ? STA_IMAGE_ENDIAN_LITTLE : STA_IMAGE_ENDIAN_BIG;
    assert(hdr.endian == expected_endian);

    /* Pointer width. */
    assert(hdr.ptr_width == 8);

    /* Object count must be positive. */
    assert(hdr.object_count > 0);

    /* Immutable count: at least nil, true, false + some symbols. */
    assert(hdr.immutable_count > 0);

    /* Section offsets within file. */
    assert(hdr.immutable_section_offset == 48);
    assert(hdr.data_section_offset > hdr.immutable_section_offset);
    assert(hdr.reloc_section_offset > hdr.data_section_offset);
    assert(hdr.file_size >= hdr.reloc_section_offset);

    /* file_size matches actual file. */
    FILE *f2 = fopen(image_path, "rb");
    fseek(f2, 0, SEEK_END);
    assert((uint64_t)ftell(f2) == hdr.file_size);
    fclose(f2);
}

static void test_immutable_section_has_nil_true_false(void) {
    FILE *f = fopen(image_path, "rb");
    assert(f);
    STA_ImageHeader hdr;
    assert(fread(&hdr, sizeof(hdr), 1, f) == 1);

    /* Seek to immutable section. */
    assert(fseek(f, (long)hdr.immutable_section_offset, SEEK_SET) == 0);

    uint32_t nil_key   = sta_fnv1a("nil", 3);
    uint32_t true_key  = sta_fnv1a("true", 4);
    uint32_t false_key = sta_fnv1a("false", 5);
    bool found_nil = false, found_true = false, found_false = false;

    char name_buf[256];
    for (uint32_t i = 0; i < hdr.immutable_count; i++) {
        STA_ImmutableEntry ent;
        assert(fread(&ent, sizeof(ent), 1, f) == 1);
        assert(ent.name_len < sizeof(name_buf));
        if (ent.name_len > 0) {
            assert(fread(name_buf, 1, ent.name_len, f) == ent.name_len);
        }

        if (ent.stable_key == nil_key && ent.name_len == 3 &&
            memcmp(name_buf, "nil", 3) == 0) found_nil = true;
        if (ent.stable_key == true_key && ent.name_len == 4 &&
            memcmp(name_buf, "true", 4) == 0) found_true = true;
        if (ent.stable_key == false_key && ent.name_len == 5 &&
            memcmp(name_buf, "false", 5) == 0) found_false = true;
    }

    fclose(f);
    assert(found_nil);
    assert(found_true);
    assert(found_false);
}

static void test_object_counts_reasonable(void) {
    FILE *f = fopen(image_path, "rb");
    assert(f);
    STA_ImageHeader hdr;
    assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
    fclose(f);

    /* After bootstrap + kernel load, we expect at least a few hundred objects
     * (classes, method dictionaries, compiled methods, symbols, etc.). */
    assert(hdr.object_count > 100);

    /* Immutable count should include nil, true, false + all interned symbols.
     * Bootstrap interns at least ~50 symbols. */
    assert(hdr.immutable_count >= 3 + 50);

    printf("[%u objects, %u immutables, %llu bytes] ",
           hdr.object_count, hdr.immutable_count, (unsigned long long)hdr.file_size);
}

static void test_cleanup(void) {
    unlink(image_path);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_image_save:\n");

    setup();

    RUN(test_save_returns_ok);
    RUN(test_file_exists_and_has_size);
    RUN(test_header_magic);
    RUN(test_header_fields);
    RUN(test_immutable_section_has_nil_true_false);
    RUN(test_object_counts_reasonable);
    RUN(test_cleanup);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
