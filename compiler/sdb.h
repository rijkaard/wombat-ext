#ifndef SDB_H_
#define SDB_H_

#include <stdint.h>

#define SDB_MAX 65536

typedef struct {
    char   *entries[SDB_MAX];
    int     count;
} SDB;

/* Load sdb.txt (one entry per line). Returns 0 on success. */
int sdb_load(SDB *sdb, const char *path);

/* Save updated sdb.txt. Returns 0 on success. */
int sdb_save(const SDB *sdb, const char *path);

/*
 * Look up a plain identifier (no prefix). If not found, append.
 * Returns the 0-based index.
 */
uint16_t sdb_intern_id(SDB *sdb, const char *name);

/*
 * Look up a string value (stored in sdb.txt with a leading '"').
 * If not found, append. Returns the 0-based index.
 */
uint16_t sdb_intern_str(SDB *sdb, const char *value);

/*
 * Look up a wide-string value (stored in sdb.txt as L"value").
 * If not found, append. Returns the 0-based index.
 */
uint16_t sdb_intern_ustr(SDB *sdb, const char *value);

#endif /* SDB_H_ */
