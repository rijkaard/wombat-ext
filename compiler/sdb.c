#define _POSIX_C_SOURCE 200809L
#include "sdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sdb_load(SDB *sdb, const char *path)
{
    memset(sdb, 0, sizeof(*sdb));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (sdb->count >= SDB_MAX) {
            fprintf(stderr, "sdb: too many entries (max %d)\n", SDB_MAX);
            fclose(f);
            return -1;
        }
        sdb->entries[sdb->count++] = strdup(line);
    }

    fclose(f);
    return 0;
}

int sdb_save(const SDB *sdb, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < sdb->count; i++)
        fprintf(f, "%s\n", sdb->entries[i]);

    fclose(f);
    return 0;
}

uint16_t sdb_intern_id(SDB *sdb, const char *name)
{
    for (int i = 0; i < sdb->count; i++) {
        const char *e = sdb->entries[i];
        if (e[0] != '"' && strcmp(e, name) == 0)
            return (uint16_t)i;
    }
    if (sdb->count >= SDB_MAX) {
        fprintf(stderr, "sdb: table full, cannot add '%s'\n", name);
        exit(1);
    }
    uint16_t idx = (uint16_t)sdb->count;
    sdb->entries[sdb->count++] = strdup(name);
    return idx;
}

uint16_t sdb_intern_str(SDB *sdb, const char *value)
{
    /* sdb.txt stores string entries as: "value" (both opening and closing quote) */
    size_t vlen = strlen(value);
    char *key = malloc(vlen + 3);
    key[0] = '"';
    memcpy(key + 1, value, vlen);
    key[vlen + 1] = '"';
    key[vlen + 2] = '\0';

    for (int i = 0; i < sdb->count; i++) {
        if (strcmp(sdb->entries[i], key) == 0) {
            free(key);
            return (uint16_t)i;
        }
    }
    if (sdb->count >= SDB_MAX) {
        fprintf(stderr, "sdb: table full, cannot add string '%s'\n", value);
        exit(1);
    }
    uint16_t idx = (uint16_t)sdb->count;
    sdb->entries[sdb->count++] = key;
    return idx;
}

uint16_t sdb_intern_ustr(SDB *sdb, const char *value)
{
    /* sdb.txt stores wide-string entries as: L"value" */
    size_t vlen = strlen(value);
    char *key = malloc(vlen + 4); /* L + " + value + " + \0 */
    key[0] = 'L';
    key[1] = '"';
    memcpy(key + 2, value, vlen);
    key[vlen + 2] = '"';
    key[vlen + 3] = '\0';

    for (int i = 0; i < sdb->count; i++) {
        if (strcmp(sdb->entries[i], key) == 0) {
            free(key);
            return (uint16_t)i;
        }
    }
    if (sdb->count >= SDB_MAX) {
        fprintf(stderr, "sdb: table full, cannot add ustring '%s'\n", value);
        exit(1);
    }
    uint16_t idx = (uint16_t)sdb->count;
    sdb->entries[sdb->count++] = key;
    return idx;
}
