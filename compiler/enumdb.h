#ifndef ENUMDB_H_
#define ENUMDB_H_

#include <stdint.h>

#define ENUMDB_MAX_ENTRIES  4096
#define ENUMDB_MAX_ANNOTS    512
#define ENUMDB_NAME_LEN       80
#define ENUMDB_TYPE_LEN       64
#define ENUMDB_CTX_DEPTH      16
#define ENUMDB_CTX_FUNC_LEN  256

typedef struct {
    char    name[ENUMDB_NAME_LEN];
    int64_t value;
    char    type_name[ENUMDB_TYPE_LEN];
} EnumEntry;

typedef struct {
    char func_name[ENUMDB_NAME_LEN];
    int  arg_index;  /* 0-based */
    char type_name[ENUMDB_TYPE_LEN];
} EnumAnnotation;

typedef struct {
    EnumEntry    entries[ENUMDB_MAX_ENTRIES];
    int          count;
    EnumAnnotation annots[ENUMDB_MAX_ANNOTS];
    int          n_annots;
    int          n_annots_file; /* annotations loaded from file; inferred follow */
} EnumDB;

/* Parse enumerations.h and populate db. Returns 0 on success. */
int enumdb_load(EnumDB *db, const char *path);

/* Load function→arg→type annotations from a text file. Returns 0 on success. */
int enumdb_load_annots(EnumDB *db, const char *path);

/* Look up an enum constant by name. Returns 1 if found, 0 if not. */
int enumdb_lookup(const EnumDB *db, const char *name,
                  int64_t *val_out, const char **type_out);

/* Get expected enum type for a function+arg. Returns NULL if no annotation. */
const char *enumdb_get_annotation(const EnumDB *db,
                                  const char *func, int arg_index);

/* Add an inferred annotation (no-op if already present or table is full). */
void enumdb_add_annot(EnumDB *db, const char *func, int arg_index,
                      const char *type);

#endif /* ENUMDB_H_ */
