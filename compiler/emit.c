#include "emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void grow(Emit *e, size_t n)
{
    if (e->len + n <= e->cap) return;
    size_t ncap = e->cap ? e->cap * 2 : 4096;
    while (ncap < e->len + n) ncap *= 2;
    e->data = realloc(e->data, ncap);
    if (!e->data) { perror("realloc"); exit(1); }
    e->cap = ncap;
}

void emit_init(Emit *e)
{
    memset(e, 0, sizeof(*e));
}

void emit_free(Emit *e)
{
    free(e->data);
    memset(e, 0, sizeof(*e));
}

void emit_tok(Emit *e, uint16_t tok)
{
    grow(e, 2);
    e->data[e->len++] = (uint8_t)(tok & 0xFF);
    e->data[e->len++] = (uint8_t)(tok >> 8);
}

static void emit_u16(Emit *e, uint16_t v)
{
    grow(e, 2);
    e->data[e->len++] = (uint8_t)(v & 0xFF);
    e->data[e->len++] = (uint8_t)(v >> 8);
}

static void emit_u32(Emit *e, uint32_t v)
{
    grow(e, 4);
    e->data[e->len++] = (uint8_t)(v & 0xFF);
    e->data[e->len++] = (uint8_t)((v >> 8) & 0xFF);
    e->data[e->len++] = (uint8_t)((v >> 16) & 0xFF);
    e->data[e->len++] = (uint8_t)((v >> 24) & 0xFF);
}

void emit_id(Emit *e, SDB *sdb, const char *name)
{
    uint16_t idx = sdb_intern_id(sdb, name);
    emit_tok(e, BIN_T_ID);
    emit_u16(e, idx);
}

void emit_str(Emit *e, SDB *sdb, const char *value)
{
    uint16_t idx = sdb_intern_str(sdb, value);
    emit_tok(e, BIN_T_STR);
    emit_u16(e, idx);
}

void emit_ustr(Emit *e, SDB *sdb, const char *value)
{
    uint16_t idx = sdb_intern_ustr(sdb, value);
    emit_tok(e, BIN_T_STR);
    emit_u16(e, idx);
}

void emit_int(Emit *e, int64_t value, int hexdigits)
{
    /* Hex literals: digit count determines size (1-2→byte, 3-4→word, 5+→dword).
     * Decimal literals (hexdigits==0): use value magnitude. */
    if (hexdigits >= 5) {
        emit_tok(e, BIN_T_DWORD);
        emit_u32(e, (uint32_t)(int32_t)value);
    } else if (hexdigits >= 3) {
        emit_tok(e, BIN_T_WORD);
        emit_u16(e, (uint16_t)value);
    } else if (hexdigits >= 1) {
        emit_tok(e, BIN_T_BYTE);
        grow(e, 1);
        e->data[e->len++] = (uint8_t)value;
    } else if (value >= 0 && value <= 0xFF) {
        emit_tok(e, BIN_T_BYTE);
        grow(e, 1);
        e->data[e->len++] = (uint8_t)value;
    } else if (value >= 0 && value <= 0xFFFF) {
        emit_tok(e, BIN_T_WORD);
        emit_u16(e, (uint16_t)value);
    } else {
        emit_tok(e, BIN_T_DWORD);
        emit_u32(e, (uint32_t)(int32_t)value);
    }
}

void emit_null(Emit *e)
{
    grow(e, 1);
    e->data[e->len++] = 0;
}

int emit_write(const Emit *e, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(e->data, 1, e->len, f) != e->len) {
        perror(path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
