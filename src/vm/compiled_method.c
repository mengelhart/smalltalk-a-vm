/* src/vm/compiled_method.c
 * CompiledMethod builder — see compiled_method.h for documentation.
 */
#include "compiled_method.h"
#include "class_table.h"
#include <string.h>

STA_OOP sta_compiled_method_create(STA_ImmutableSpace *sp,
    uint8_t numArgs, uint8_t numTemps, uint8_t primIndex,
    const STA_OOP *literals, uint8_t numLiterals,
    const uint8_t *bytecodes, uint32_t numBytecodes)
{
    /* Compute total payload words:
     *   1 (header word) + numLiterals + ceil(numBytecodes / 8) */
    uint32_t bytecode_words = (numBytecodes + 7u) / 8u;
    uint32_t nwords = 1 + (uint32_t)numLiterals + bytecode_words;

    STA_ObjHeader *h = sta_immutable_alloc(sp, STA_CLS_COMPILEDMETHOD, nwords);
    if (!h) return 0;

    STA_OOP *payload = sta_payload(h);

    /* Header word. */
    uint8_t has_prim = (primIndex != 0) ? 1 : 0;
    payload[0] = STA_METHOD_HEADER(numArgs, numTemps, numLiterals,
                                   primIndex, has_prim, 0);

    /* Literal frame. */
    for (uint8_t i = 0; i < numLiterals; i++) {
        payload[1 + i] = literals[i];
    }

    /* Bytecodes — copy raw bytes into the remaining payload area. */
    if (numBytecodes > 0) {
        uint8_t *bc_dest = (uint8_t *)&payload[1 + numLiterals];
        memcpy(bc_dest, bytecodes, numBytecodes);
    }

    return (STA_OOP)(uintptr_t)h;
}

STA_OOP sta_compiled_method_create_with_header(STA_ImmutableSpace *sp,
    STA_OOP header,
    const STA_OOP *literals, uint8_t numLiterals,
    const uint8_t *bytecodes, uint32_t numBytecodes)
{
    uint32_t bytecode_words = (numBytecodes + 7u) / 8u;
    uint32_t nwords = 1 + (uint32_t)numLiterals + bytecode_words;

    STA_ObjHeader *h = sta_immutable_alloc(sp, STA_CLS_COMPILEDMETHOD, nwords);
    if (!h) return 0;

    STA_OOP *payload = sta_payload(h);
    payload[0] = header;

    for (uint8_t i = 0; i < numLiterals; i++) {
        payload[1 + i] = literals[i];
    }

    if (numBytecodes > 0) {
        uint8_t *bc_dest = (uint8_t *)&payload[1 + numLiterals];
        memcpy(bc_dest, bytecodes, numBytecodes);
    }

    return (STA_OOP)(uintptr_t)h;
}
