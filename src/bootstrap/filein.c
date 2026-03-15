/* src/bootstrap/filein.c
 * Squeak/Pharo chunk-format file-in reader — see filein.h for documentation.
 *
 * SCAFFOLDING: This C file-in reader is deliberately simple bootstrap
 * infrastructure. It will be replaced by a Smalltalk-level implementation
 * in Phase 3+ once FileStream and Compiler are available as Smalltalk
 * objects. Do not over-engineer this code.
 */
#include "filein.h"
#include "../vm/interpreter.h"
#include "../vm/compiled_method.h"
#include "../vm/method_dict.h"
#include "../vm/special_objects.h"
#include "../vm/format.h"
#include "../vm/frame.h"
#include "../vm/primitive_table.h"
#include "../compiler/compiler.h"
#include "../compiler/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Error codes ──────────────────────────────────────────────────────── */

#define FILEIN_OK         0
#define FILEIN_ERR_IO    (-3)  /* matches STA_ERR_IO */
#define FILEIN_ERR_COMPILE (-5)

/* ── Chunk reader ─────────────────────────────────────────────────────── */

/* Read an entire file into a malloc'd buffer. Returns NULL on failure. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);

    buf[read] = '\0';
    *out_len = read;
    return buf;
}

/* Extract the next chunk from source starting at *pos.
 * A chunk is delimited by an unescaped '!'. Double '!!' is escaped to '!'.
 * Returns a malloc'd string with unescaped content, or NULL at end.
 * Sets *out_len to the unescaped length. */
static char *next_chunk(const char *source, size_t source_len,
                         size_t *pos, size_t *out_len) {
    if (*pos >= source_len) return NULL;

    /* Skip leading whitespace and newlines between chunks. */
    while (*pos < source_len &&
           (source[*pos] == '\n' || source[*pos] == '\r' ||
            source[*pos] == ' '  || source[*pos] == '\t')) {
        (*pos)++;
    }
    if (*pos >= source_len) return NULL;

    /* Scan for unescaped '!'. */
    size_t start = *pos;
    size_t cap = source_len - start;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;

    size_t out = 0;
    size_t i = start;
    while (i < source_len) {
        if (source[i] == '!') {
            if (i + 1 < source_len && source[i + 1] == '!') {
                /* Escaped '!' — emit single '!'. */
                buf[out++] = '!';
                i += 2;
            } else {
                /* Unescaped '!' — end of chunk. */
                i++; /* skip the '!' */
                break;
            }
        } else {
            buf[out++] = source[i++];
        }
    }

    *pos = i;
    buf[out] = '\0';
    *out_len = out;

    /* If the chunk is empty (just whitespace), return empty string. */
    return buf;
}

/* ── Chunk type identification ────────────────────────────────────────── */

static int is_class_definition(const char *chunk) {
    return strstr(chunk, "subclass:") != NULL &&
           strstr(chunk, "instanceVariableNames:") != NULL;
}

/* Parse a methodsFor header: "ClassName methodsFor: 'category'"
 * Extracts the class name into out_class (caller-provided buffer).
 * Returns 1 on success, 0 if not a methodsFor header. */
static int parse_methods_for(const char *chunk, char *out_class, size_t class_buf_len) {
    /* Pattern: "ClassName methodsFor: 'category'" */
    const char *mf = strstr(chunk, " methodsFor:");
    if (!mf) return 0;

    size_t name_len = (size_t)(mf - chunk);
    if (name_len == 0 || name_len >= class_buf_len) return 0;

    memcpy(out_class, chunk, name_len);
    out_class[name_len] = '\0';
    return 1;
}

/* ── Helper: look up class in SystemDictionary by name ────────────────── */

static STA_OOP lookup_class(STA_FileInContext *ctx, const char *name) {
    STA_OOP dict = sta_spc_get(SPC_SMALLTALK);
    if (dict == 0) return 0;

    STA_OOP name_sym = sta_symbol_intern(ctx->immutable_space,
                                           ctx->symbol_table,
                                           name, strlen(name));
    if (name_sym == 0) return 0;

    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP arr = sta_payload(dh)[1];
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    uint32_t cap = ah->size / 2;
    STA_OOP *slots = sta_payload(ah);

    uint32_t hash = sta_symbol_get_hash(name_sym);
    uint32_t idx = hash % cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == name_sym) {
            STA_OOP assoc = slots[pos + 1];
            return sta_payload((STA_ObjHeader *)(uintptr_t)assoc)[1];
        }
        if (slots[pos] == 0) break;
    }
    return 0;
}

/* ── Helper: parse and record instvar names from class definitions ────── */

static void record_class_info(STA_FileInContext *ctx, const char *chunk) {
    if (ctx->class_info_count >= STA_FILEIN_MAX_CLASSES) return;

    STA_FileInClassInfo *info = &ctx->class_info[ctx->class_info_count];
    memset(info, 0, sizeof(*info));

    const char *sc = strstr(chunk, "subclass:");
    if (!sc) return;
    sc += 9;
    while (*sc == ' ' || *sc == '\t' || *sc == '\n' || *sc == '\r') sc++;
    if (*sc == '#') sc++;
    size_t nlen = 0;
    while (sc[nlen] && sc[nlen] != ' ' && sc[nlen] != '\n' &&
           sc[nlen] != '\r' && sc[nlen] != '\t') nlen++;
    if (nlen == 0 || nlen >= sizeof(info->class_name)) return;
    memcpy(info->class_name, sc, nlen);
    info->class_name[nlen] = '\0';

    const char *sub = strstr(chunk, " subclass:");
    if (sub) {
        const char *p = chunk;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        size_t slen = (size_t)(sub - p);
        if (slen > 0 && slen < sizeof(info->super_name)) {
            memcpy(info->super_name, p, slen);
            info->super_name[slen] = '\0';
        }
    }

    const char *ivn = strstr(chunk, "instanceVariableNames:");
    if (ivn) {
        ivn += 22;
        const char *q = strchr(ivn, '\'');
        if (q) {
            q++;
            const char *end = strchr(q, '\'');
            if (end) {
                while (q < end && info->ivar_count < STA_FILEIN_MAX_IVARS) {
                    while (q < end && (*q == ' ' || *q == '\t')) q++;
                    if (q >= end) break;
                    const char *start = q;
                    while (q < end && *q != ' ' && *q != '\t') q++;
                    size_t len = (size_t)(q - start);
                    if (len > 0 && len < 64) {
                        memcpy(info->ivar_names[info->ivar_count], start, len);
                        info->ivar_names[info->ivar_count][len] = '\0';
                        info->ivar_count++;
                    }
                }
            }
        }
    }

    ctx->class_info_count++;
}

static STA_FileInClassInfo *find_class_info(STA_FileInContext *ctx,
                                              const char *name) {
    for (uint32_t i = 0; i < ctx->class_info_count; i++) {
        if (strcmp(ctx->class_info[i].class_name, name) == 0)
            return &ctx->class_info[i];
    }
    return NULL;
}

static uint32_t collect_instvar_names(STA_FileInContext *ctx,
                                        const char *class_name,
                                        const char **out_names,
                                        uint32_t max_names) {
    const char *chain[32];
    uint32_t chain_len = 0;
    const char *cur = class_name;
    while (cur && chain_len < 32) {
        STA_FileInClassInfo *ci = find_class_info(ctx, cur);
        if (!ci) break;
        chain[chain_len++] = cur;
        cur = (ci->super_name[0] != '\0') ? ci->super_name : NULL;
    }
    uint32_t count = 0;
    for (int i = (int)chain_len - 1; i >= 0; i--) {
        STA_FileInClassInfo *ci = find_class_info(ctx, chain[i]);
        if (!ci) continue;
        for (uint32_t j = 0; j < ci->ivar_count && count < max_names; j++) {
            out_names[count++] = ci->ivar_names[j];
        }
    }
    return count;
}

/* ── Execute a class definition chunk ─────────────────────────────────── */

static int execute_class_definition(STA_FileInContext *ctx,
                                      const char *chunk, int chunk_num) {
    /* Record instvar names for later use by the compiler. */
    record_class_info(ctx, chunk);
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);

    /* Compile the class definition as an expression. */
    STA_CompileResult cr = sta_compile_expression(
        chunk, ctx->symbol_table, ctx->immutable_space,
        ctx->heap, sysdict);

    if (cr.had_error) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: class definition compile error: %s",
                 chunk_num, cr.error_msg);
        return FILEIN_ERR_COMPILE;
    }

    /* Execute via interpreter. */
    STA_StackSlab *slab = sta_stack_slab_create(64 * 1024);
    if (!slab) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: out of memory allocating stack slab", chunk_num);
        return FILEIN_ERR_COMPILE;
    }

    sta_primitive_set_slab(slab);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    (void)sta_interpret(slab, ctx->heap, ctx->class_table,
                         cr.method, nil_oop, NULL, 0);

    sta_stack_slab_destroy(slab);
    return FILEIN_OK;
}

/* ── Compile and install a method ─────────────────────────────────────── */

static int install_method(STA_FileInContext *ctx, const char *class_name,
                            const char *source, int chunk_num) {
    STA_OOP class_oop = lookup_class(ctx, class_name);
    if (class_oop == 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: class '%s' not found", chunk_num, class_name);
        return FILEIN_ERR_COMPILE;
    }

    /* Parse the method to extract the selector name. */
    STA_Parser parser;
    STA_AstNode *ast = sta_parse_method(source, &parser);
    if (!ast) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: parse error in %s: %s",
                 chunk_num, class_name,
                 parser.had_error ? parser.error_msg : "unknown");
        return FILEIN_ERR_COMPILE;
    }
    const char *selector_str = ast->as.method.selector;

    /* Intern the selector. */
    STA_OOP selector = sta_symbol_intern(ctx->immutable_space,
                                           ctx->symbol_table,
                                           selector_str, strlen(selector_str));
    if (selector == 0) {
        sta_ast_free(ast);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: failed to intern selector '%s'",
                 chunk_num, selector_str);
        return FILEIN_ERR_COMPILE;
    }

    sta_ast_free(ast);

    /* Collect instvar names for the class (walking superclass chain). */
    const char *ivar_ptrs[64];
    uint32_t ivar_count = collect_instvar_names(ctx, class_name,
                                                  ivar_ptrs, 64);

    /* Compile the method. */
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_method(
        source, class_oop, ivar_ptrs, ivar_count,
        ctx->symbol_table, ctx->immutable_space,
        ctx->heap, sysdict);

    if (cr.had_error) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: method compile error in %s: %s",
                 chunk_num, class_name, cr.error_msg);
        return FILEIN_ERR_COMPILE;
    }

    /* Install in the class's method dictionary. */
    STA_OOP md = sta_class_method_dict(class_oop);
    int rc = sta_method_dict_insert(ctx->heap, md, selector, cr.method);
    if (rc != 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "chunk %d: failed to install method in %s", chunk_num, class_name);
        return FILEIN_ERR_COMPILE;
    }

    return FILEIN_OK;
}

/* ── Main file-in logic ───────────────────────────────────────────────── */

int sta_filein_load(STA_FileInContext *ctx, const char *path) {
    ctx->error_msg[0] = '\0';

    size_t source_len = 0;
    char *source = read_file(path, &source_len);
    if (!source) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "cannot open file: %s", path);
        return FILEIN_ERR_IO;
    }

    size_t pos = 0;
    int chunk_num = 0;
    int rc = FILEIN_OK;

    /* State: when inside a methodsFor section, this holds the class name. */
    char current_class[256] = {0};
    int in_methods = 0;

    while (pos < source_len) {
        size_t chunk_len = 0;
        char *chunk = next_chunk(source, source_len, &pos, &chunk_len);
        if (!chunk) break;

        chunk_num++;

        /* Skip empty chunks (they terminate method sections). */
        if (chunk_len == 0 ||
            (chunk_len == 1 && (chunk[0] == '\n' || chunk[0] == '\r'))) {
            in_methods = 0;
            free(chunk);
            continue;
        }

        /* Trim leading/trailing whitespace for identification. */
        const char *trimmed = chunk;
        while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r' ||
               *trimmed == '\t')
            trimmed++;

        size_t trimmed_len = strlen(trimmed);
        while (trimmed_len > 0 &&
               (trimmed[trimmed_len - 1] == ' ' ||
                trimmed[trimmed_len - 1] == '\n' ||
                trimmed[trimmed_len - 1] == '\r' ||
                trimmed[trimmed_len - 1] == '\t'))
            trimmed_len--;

        if (trimmed_len == 0) {
            in_methods = 0;
            free(chunk);
            continue;
        }

        if (in_methods) {
            /* This chunk is a method body for current_class. */
            rc = install_method(ctx, current_class, chunk, chunk_num);
            if (rc != FILEIN_OK) { free(chunk); break; }
        } else if (is_class_definition(chunk)) {
            rc = execute_class_definition(ctx, chunk, chunk_num);
            if (rc != FILEIN_OK) { free(chunk); break; }
        } else {
            char class_buf[256];
            if (parse_methods_for(trimmed, class_buf, sizeof(class_buf))) {
                strncpy(current_class, class_buf, sizeof(current_class) - 1);
                current_class[sizeof(current_class) - 1] = '\0';
                in_methods = 1;
            }
            /* else: unknown chunk type — skip silently for Phase 1. */
        }

        free(chunk);
    }

    free(source);
    return rc;
}
