#ifndef EMIT_H_
#define EMIT_H_

#include "tokens.h"
#include "sdb.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Emit;

void     emit_init(Emit *e);
void     emit_free(Emit *e);

/* Write a 2-byte variant-0 token (little-endian). */
void     emit_tok(Emit *e, uint16_t tok);

/* T_ID: 2-byte token + 2-byte SDB index. */
void     emit_id(Emit *e, SDB *sdb, const char *name);

/* T_STR: 2-byte token + 2-byte SDB index (value stored with '"' prefix). */
void     emit_str(Emit *e, SDB *sdb, const char *value);

/* T_STR (wide): 2-byte token + 2-byte SDB index (value stored as L"..."). */
void     emit_ustr(Emit *e, SDB *sdb, const char *value);

/* T_BYTE/T_WORD/T_DWORD selected by hex digit count (0=decimal→magnitude). */
void     emit_int(Emit *e, int64_t value, int hexdigits);

/* Terminating NUL byte. */
void     emit_null(Emit *e);

/* Write buffer to file. Returns 0 on success. */
int      emit_write(const Emit *e, const char *path);

#endif /* EMIT_H_ */
