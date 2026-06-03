#ifndef SYMTAB_H_
#define SYMTAB_H_

typedef struct {
    char **names;
    int    count, cap;
} NameSet;

/* Stores the declared parameter count of a user-defined function. */
typedef struct {
    char *name;
    int   param_count;
} FuncSig;

typedef struct {
    FuncSig *sigs;
    int      count, cap;
} FuncSigSet;

typedef struct {
    NameSet    members;         /* own + inherited member variables        */
    NameSet    funcs;           /* functions defined/forward-declared here */
    NameSet    engine;          /* engine API functions (from file)        */
    NameSet    locals;          /* params + locals for current scope       */
    FuncSigSet user_sigs;       /* param counts for user-defined functions */
    int        locals_base;     /* locals.count at scope entry             */
    int        in_body;         /* non-zero when inside a function/trigger */
    int        has_engine_api;  /* non-zero when engine-api.txt was loaded */
    int        errors;          /* total undefined-symbol errors           */
    char       parent[256];     /* script named in `inherits` (if any)     */
    const char *script;         /* script basename (for diagnostics)       */
} SymTab;

void symtab_init   (SymTab *t, const char *script);
void symtab_free   (SymTab *t);

/* Load engine-api.txt; returns 0 on success, -1 on error. */
int  symtab_load_engine   (SymTab *t, const char *path);

/*
 * Load member-db.txt and add the members visible in `parent` (and its
 * ancestors, which are already flattened by gen-member-db.mjs) to
 * t->members.  Returns 0 on success.
 */
int  symtab_load_inherited(SymTab *t, const char *db_path);

/*
 * Find <scripts_dir>/<parent_name>.m, scan its member/function declarations
 * into t, then recursively follow its own inherits chain (max 16 levels).
 * Returns 0 on success, -1 if the file cannot be opened.
 * Preferred over symtab_load_inherited when source files are available.
 */
int  symtab_load_parent_from_dir(SymTab *t, const char *scripts_dir,
                                  const char *parent_name, int depth);

/*
 * Pre-scan `src` using a second lexer pass.  Populates t->members,
 * t->funcs, and t->parent.  Call before symtab_load_inherited().
 */
void symtab_prescan(SymTab *t, const char *src);

/* Register a locally-declared variable or parameter. */
void symtab_add_local(SymTab *t, const char *name);

/* Enter a new function/trigger scope. */
void symtab_push_scope(SymTab *t);

/* Leave the current scope (discards locals added since push). */
void symtab_pop_scope(SymTab *t);

/*
 * Check that `name` is a known symbol.  `is_call` is non-zero when the
 * identifier is used in call position (followed by '(').
 * Emits a diagnostic to stderr and increments t->errors if undefined.
 */
void symtab_check(SymTab *t, const char *name, int line, int is_call);

/*
 * After parsing arguments, check that `argc` matches the declared parameter
 * count for `name`.  Does nothing if `name` is not in the user_sigs table
 * (engine API calls are not checked).
 */
void symtab_check_call_argc(SymTab *t, const char *name, int line, int argc);

#endif /* SYMTAB_H_ */
