#include "tokens.h"
#include "sdb.h"
#include "emit.h"
#include "lex.h"
#include "symtab.h"
#include "enumdb.h"
#include "funcdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void compile(Lexer *lex, Emit *emit, SDB *sdb, SymTab *symtab, EnumDB *enumdb);

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    sz = (long)fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Extract "basename.ext" → "basename" from a path. */
static const char *script_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-sdb sdb.txt] [-o output.m] [-update-sdb out_sdb.txt]\n"
        "          [-engine-api path] [-member-db path]\n"
        "          [-enums enumerations.h] [-enum-annots annotations.txt]\n"
        "          [-annotate-only] input.m\n"
        "\n"
        "  -sdb <path>          SDB file to load (default: sdb.txt in current dir)\n"
        "  -o <path>            Output binary path (default: <input>.out)\n"
        "  -update-sdb <path>   Write updated SDB here (default: same as -sdb)\n"
        "  -engine-api <path>   Engine API list for symbol checking\n"
        "  -member-db  <path>   Member database for symbol checking\n"
        "  -enums <path>        enumerations.h to enable enum constant support\n"
        "  -enum-annots <path>  Annotation file for enum type validation\n"
        "  -annotate-only       Infer user-function enum annotations and print them;\n"
        "                       skip bytecode compilation (requires -enums)\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *sdb_in      = "sdb.txt";
    const char *sdb_out     = NULL;
    const char *out         = NULL;
    const char *input       = NULL;
    const char *engine_api  = NULL;
    const char *member_db   = NULL;
    const char *enums_path  = NULL;
    const char *annots_path = NULL;
    int         annotate_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-sdb") == 0 && i+1 < argc) {
            sdb_in = argv[++i];
        } else if (strcmp(argv[i], "-update-sdb") == 0 && i+1 < argc) {
            sdb_out = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "-engine-api") == 0 && i+1 < argc) {
            engine_api = argv[++i];
        } else if (strcmp(argv[i], "-member-db") == 0 && i+1 < argc) {
            member_db = argv[++i];
        } else if (strcmp(argv[i], "-enums") == 0 && i+1 < argc) {
            enums_path = argv[++i];
        } else if (strcmp(argv[i], "-enum-annots") == 0 && i+1 < argc) {
            annots_path = argv[++i];
        } else if (strcmp(argv[i], "-annotate-only") == 0) {
            annotate_only = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            input = argv[i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!input) { usage(argv[0]); return 1; }

    /* Default output path */
    char default_out[4096];
    if (!out) {
        size_t len = strlen(input);
        if (len > 4096 - 5) len = 4096 - 5;
        memcpy(default_out, input, len);
        /* Strip extension if present */
        char *dot = strrchr(default_out, '.');
        if (dot) *dot = '\0';
        else default_out[len] = '\0';
        strncat(default_out, ".out", sizeof(default_out) - strlen(default_out) - 1);
        out = default_out;
    }

    if (!sdb_out) sdb_out = sdb_in;

    /* Heap-allocate SDB: ~512 KB on 64-bit, too large for the default Windows stack (1 MB) */
    SDB *sdb = calloc(1, sizeof(*sdb));
    if (!sdb) { fprintf(stderr, "out of memory\n"); return 1; }
    if (sdb_load(sdb, sdb_in) != 0) {
        fprintf(stderr, "warning: could not load SDB from '%s', starting empty\n", sdb_in);
    }

    /* Read source */
    char *src = read_file(input);
    if (!src) return 1;

    /* Always build symbol table; engine-api enables function-call validation */
    SymTab symtab_storage;
    symtab_init(&symtab_storage, script_basename(input));
    symtab_prescan(&symtab_storage, src);

    if (engine_api) {
        if (symtab_load_engine(&symtab_storage, engine_api) != 0) {
            fprintf(stderr, "error: cannot load engine-api from '%s'\n", engine_api);
            free(src);
            return 1;
        }
    }

    /* Load inherited members: prefer reading parent .m file from same directory
     * as the input (works for both Q-coded and decoded script trees), fall back
     * to member-db when the file isn't found next to the input. */
    if (symtab_storage.parent[0] != '\0') {
        /* Derive the directory containing the input file */
        char scripts_dir[4096];
        const char *slash = strrchr(input, '/');
        if (!slash) slash = strrchr(input, '\\');
        size_t dirlen = slash ? (size_t)(slash - input) : 0;
        if (dirlen >= sizeof(scripts_dir)) dirlen = sizeof(scripts_dir) - 1;
        memcpy(scripts_dir, input, dirlen);
        scripts_dir[dirlen] = '\0';
        if (dirlen == 0) { scripts_dir[0] = '.'; scripts_dir[1] = '\0'; }

        if (symtab_load_parent_from_dir(&symtab_storage, scripts_dir,
                                         symtab_storage.parent, 16) != 0) {
            /* File not found next to input — try member-db if supplied */
            if (member_db)
                symtab_load_inherited(&symtab_storage, member_db);
        }
    }

    SymTab *symtab = &symtab_storage;

    /* Heap-allocate EnumDB (~682 KB) and FuncDB (~6 MB) — both too large for
     * the default Windows stack (1 MB). */
    EnumDB *enumdb = NULL;
    if (enums_path) {
        enumdb = calloc(1, sizeof(*enumdb));
        if (!enumdb) { fprintf(stderr, "out of memory\n"); free(src); free(sdb); return 1; }
        if (enumdb_load(enumdb, enums_path) != 0) {
            fprintf(stderr, "error: cannot load enums from '%s'\n", enums_path);
            free(src); free(sdb); free(enumdb);
            return 1;
        }
        if (annots_path) {
            if (enumdb_load_annots(enumdb, annots_path) != 0) {
                fprintf(stderr, "warning: cannot load enum annotations from '%s'\n",
                        annots_path);
            }
        }
        fprintf(stderr, "enums: %d constants loaded", enumdb->count);
        if (annots_path) fprintf(stderr, ", %d annotations", enumdb->n_annots);
        fprintf(stderr, "\n");

        /* Infer user-function enum annotations via param-forwarding analysis. */
        FuncDB *funcdb = calloc(1, sizeof(*funcdb));
        if (!funcdb) { fprintf(stderr, "out of memory\n"); free(src); free(sdb); free(enumdb); return 1; }
        funcdb_prescan(funcdb, src);
        int n_inferred = funcdb_propagate(funcdb, enumdb);
        if (n_inferred > 0 || annotate_only) {
            fprintf(stderr, "funcdb: %d user funcs scanned, %d annotations inferred\n",
                    funcdb->n_defs, n_inferred);
        }
        free(funcdb);

        if (annotate_only) {
            funcdb_print_annots(enumdb, stdout);
            free(src); free(sdb); free(enumdb);
            return 0;
        }
    }

    /* Compile */
    Emit  emit;
    Lexer lex;
    emit_init(&emit);
    lex_init(&lex, src);
    compile(&lex, &emit, sdb, symtab, enumdb);
    free(src);

    /* Fail if symbol errors were found */
    if (symtab->errors > 0) {
        fprintf(stderr, "%s: %d undefined symbol(s)\n", input, symtab->errors);
        symtab_free(&symtab_storage);
        emit_free(&emit);
        free(sdb); free(enumdb);
        return 1;
    }

    symtab_free(&symtab_storage);

    /* Write binary */
    if (emit_write(&emit, out) != 0) { free(sdb); free(enumdb); return 1; }
    emit_free(&emit);

    /* Write SDB if changed */
    if (strcmp(sdb_out, sdb_in) != 0 || 1 /* always write */) {
        if (sdb_save(sdb, sdb_out) != 0) {
            fprintf(stderr, "warning: could not write SDB to '%s'\n", sdb_out);
        }
    }

    printf("compiled %s → %s  (SDB: %d entries)\n",
           input, out, sdb->count);
    free(sdb); free(enumdb);
    return 0;
}
