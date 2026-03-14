/* src/vm/selector.c
 * Selector arity helper — see selector.h for documentation.
 */
#include "selector.h"
#include <string.h>

/* Binary selector special characters per bytecode spec §4.10. */
static int is_binary_char(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '<' || c == '>' || c == '=' || c == '~' ||
           c == '@' || c == '%' || c == '&' || c == '|' ||
           c == '!' || c == '?' || c == '\\' || c == ',';
}

uint8_t sta_selector_arity(STA_OOP selector) {
    size_t len;
    const char *bytes = sta_symbol_get_bytes(selector, &len);

    if (len == 0) return 0;

    /* Binary: first char is a special character. */
    if (is_binary_char(bytes[0])) return 1;

    /* Keyword: count colons. */
    uint8_t colons = 0;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == ':') colons++;
    }
    return colons;  /* 0 for unary (no colons) */
}
