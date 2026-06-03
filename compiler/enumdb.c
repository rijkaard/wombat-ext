#include "enumdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void skip_ws(const char **p)
{
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

static int parse_int64(const char *s, int64_t *out)
{
    if (!*s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }

    /* char literal: 'd' → value of 'd' */
    if (*s == '\'') {
        *out = neg ? -(int64_t)(unsigned char)s[1] : (int64_t)(unsigned char)s[1];
        return 1;
    }

    char *end;
    uint64_t v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = strtoull(s, &end, 16);
    else
        v = strtoull(s, &end, 10);

    if (end == s) return 0;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}

/* Strip block and line comments from `line` in-place using a persistent state.
 * `in_block` carries cross-line block-comment state. */
static void strip_comments(char *line, int *in_block)
{
    char *w = line;
    for (char *r = line; *r; r++) {
        if (*in_block) {
            if (r[0] == '*' && r[1] == '/') { *in_block = 0; r++; }
        } else {
            if (r[0] == '/' && r[1] == '*') { *in_block = 1; r++; }
            else if (r[0] == '/' && r[1] == '/') { break; }
            else { *w++ = *r; }
        }
    }
    *w = '\0';
}

/* ── public API ──────────────────────────────────────────────────────────── */

int enumdb_load(EnumDB *db, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    db->count    = 0;
    db->n_annots = 0;

    char   line[512];
    char   cur_type[ENUMDB_TYPE_LEN] = "";
    int    in_enum   = 0;
    int    in_block  = 0;   /* block-comment state */
    int64_t last_val = -1;

    while (fgets(line, sizeof(line), f)) {
        strip_comments(line, &in_block);

        const char *p = line;
        skip_ws(&p);
        if (!*p) continue;

        if (!in_enum) {
            /* Detect: enum TypeName { */
            if (strncmp(p, "enum", 4) == 0 && isspace((unsigned char)p[4])) {
                p += 4;
                skip_ws(&p);
                int i = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')
                       && i < ENUMDB_TYPE_LEN - 1)
                    cur_type[i++] = *p++;
                cur_type[i] = '\0';
                if (strchr(p, '{')) { in_enum = 1; last_val = -1; }
            }
        } else {
            /* Inside enum body */
            if (*p == '}') { in_enum = 0; cur_type[0] = '\0'; continue; }

            /* Only lines starting with an uppercase letter or '_' are constants */
            if (!isupper((unsigned char)*p) && *p != '_') continue;

            char name[ENUMDB_NAME_LEN];
            int  i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')
                   && i < ENUMDB_NAME_LEN - 1)
                name[i++] = *p++;
            name[i] = '\0';
            if (i == 0) continue;

            skip_ws(&p);

            int64_t val;
            if (*p == '=') {
                p++;
                skip_ws(&p);
                if (!parse_int64(p, &val)) continue;
            } else if (*p == ',' || *p == '\0') {
                /* implicit sequential value */
                val = last_val + 1;
            } else {
                continue;
            }

            last_val = val;

            if (db->count < ENUMDB_MAX_ENTRIES) {
                EnumEntry *e = &db->entries[db->count++];
                strncpy(e->name,      name,     ENUMDB_NAME_LEN - 1);
                e->name[ENUMDB_NAME_LEN - 1] = '\0';
                e->value = val;
                strncpy(e->type_name, cur_type, ENUMDB_TYPE_LEN - 1);
                e->type_name[ENUMDB_TYPE_LEN - 1] = '\0';
            }
        }
    }

    fclose(f);
    return 0;
}

int enumdb_load_annots(EnumDB *db, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *cm = strchr(line, '#');
        if (cm) *cm = '\0';

        char func[ENUMDB_NAME_LEN], type[ENUMDB_TYPE_LEN];
        int  argidx;
        if (sscanf(line, "%79s %d %63s", func, &argidx, type) == 3) {
            if (db->n_annots < ENUMDB_MAX_ANNOTS) {
                EnumAnnotation *a = &db->annots[db->n_annots++];
                strncpy(a->func_name, func, ENUMDB_NAME_LEN - 1);
                a->func_name[ENUMDB_NAME_LEN - 1] = '\0';
                a->arg_index = argidx;
                strncpy(a->type_name, type, ENUMDB_TYPE_LEN - 1);
                a->type_name[ENUMDB_TYPE_LEN - 1] = '\0';
            }
        }
    }

    fclose(f);
    db->n_annots_file = db->n_annots;
    return 0;
}

int enumdb_lookup(const EnumDB *db, const char *name,
                  int64_t *val_out, const char **type_out)
{
    for (int i = 0; i < db->count; i++) {
        if (strcmp(db->entries[i].name, name) == 0) {
            if (val_out)  *val_out  = db->entries[i].value;
            if (type_out) *type_out = db->entries[i].type_name;
            return 1;
        }
    }
    return 0;
}

const char *enumdb_get_annotation(const EnumDB *db,
                                  const char *func, int arg_index)
{
    for (int i = 0; i < db->n_annots; i++) {
        if (db->annots[i].arg_index == arg_index &&
            strcmp(db->annots[i].func_name, func) == 0)
            return db->annots[i].type_name;
    }
    return NULL;
}

void enumdb_add_annot(EnumDB *db, const char *func, int arg_index,
                      const char *type)
{
    /* No-op if already annotated. */
    for (int i = 0; i < db->n_annots; i++) {
        if (db->annots[i].arg_index == arg_index &&
            strcmp(db->annots[i].func_name, func) == 0)
            return;
    }
    if (db->n_annots >= ENUMDB_MAX_ANNOTS) return;
    EnumAnnotation *a = &db->annots[db->n_annots++];
    strncpy(a->func_name, func, ENUMDB_NAME_LEN - 1);
    a->func_name[ENUMDB_NAME_LEN - 1] = '\0';
    a->arg_index = arg_index;
    strncpy(a->type_name, type, ENUMDB_TYPE_LEN - 1);
    a->type_name[ENUMDB_TYPE_LEN - 1] = '\0';
}
