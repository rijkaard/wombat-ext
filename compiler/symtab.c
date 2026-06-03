#define _POSIX_C_SOURCE 200809L
#include "symtab.h"
#include "lex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Implicit trigger variables injected by the Wombat runtime ──────────────
 *
 * The engine binds these identifiers before a trigger fires.  They are never
 * declared in source but are always valid within a trigger body.
 *
 * 'this' is always valid in any script context (not trigger-specific).
 *
 * Trigger(s)                       Variables injected
 * ─────────────────────────────────────────────────────────────────────────
 * use / ooruse                     user
 * use_on / targetobj / oortargetobj
 *   / objaccess                    user  usedon  target  place
 * targetloc                        user  place  objtype
 * typeselected                     user  usedon  objtype  listindex
 * hueselected                      user  objhue
 * give                             givenobj  giver
 * wasgotten                        getter
 * wasdropped                       dropper
 * death         (this = victim)    attacker  corpse
 * sawdeath                         victim  attacker  corpse
 * pkpost                           killee  killer
 * washit / gotattacked /
 *   killedtarget                   attacker
 * ishitting / mobishitting         victim  damamt
 * washit                           attacker  damamt
 * speech                           speaker  arg
 * message / callback               sender  args
 * convofunc                        talker
 * textentry                        sender  button  text
 * genericgump                      entryList
 * equip                            equippedon
 * unequip                          unequippedfrom
 * lookedat                         looker
 * canbuy                           buyer
 * isstackableon                    stackon
 * multirecycle / typechange        oldtype
 * transaccountcheck /
 *   transresponse                  target  transok
 * enterrange / leaverange /
 *   time / hex-timer               target
 * ─────────────────────────────────────────────────────────────────────────
 * The checker does not track which trigger is currently being compiled, so
 * the list below is the union of all trigger sets.
 */
static const char *IMPLICIT_VARS[] = {
    /* use / targeting triggers */
    "user", "usedon", "target", "place",
    /* target-type / hue-selection */
    "objtype", "listindex", "objhue",
    /* give / get / drop */
    "givenobj", "giver", "getter", "dropper",
    /* combat */
    "victim", "attacker", "damamt", "corpse",
    /* death / pk */
    "killee", "killer",
    /* speech / message / entry */
    "speaker", "arg", "args", "sender", "talker", "text",
    /* gump */
    "button", "entryList",
    /* equip / unequip */
    "equippedon", "unequippedfrom",
    /* misc single-trigger vars */
    "looker",        /* lookedat         */
    "buyer",         /* canbuy           */
    "stackon",       /* isstackableon    */
    "oldtype",       /* multirecycle     */
    "transok",       /* transaccountcheck / transresponse */
    NULL
};

/* ── FuncSigSet helpers ──────────────────────────────────────────────────── */

static void fss_init(FuncSigSet *fss)
{
    fss->sigs = NULL;
    fss->count = fss->cap = 0;
}

static void fss_free(FuncSigSet *fss)
{
    for (int i = 0; i < fss->count; i++) free(fss->sigs[i].name);
    free(fss->sigs);
    fss_init(fss);
}

static void fss_add(FuncSigSet *fss, const char *name, int param_count)
{
    for (int i = 0; i < fss->count; i++)
        if (strcmp(fss->sigs[i].name, name) == 0) return;  /* first decl wins */
    if (fss->count == fss->cap) {
        fss->cap = fss->cap ? fss->cap * 2 : 8;
        fss->sigs = realloc(fss->sigs, (size_t)fss->cap * sizeof(FuncSig));
    }
    fss->sigs[fss->count].name        = strdup(name);
    fss->sigs[fss->count].param_count = param_count;
    fss->count++;
}

static int fss_lookup(const FuncSigSet *fss, const char *name)
{
    for (int i = 0; i < fss->count; i++)
        if (strcmp(fss->sigs[i].name, name) == 0) return fss->sigs[i].param_count;
    return -1;  /* not found */
}

/* ── NameSet helpers ─────────────────────────────────────────────────────── */

static void ns_init(NameSet *ns)
{
    ns->names = NULL;
    ns->count = ns->cap = 0;
}

static void ns_free(NameSet *ns)
{
    for (int i = 0; i < ns->count; i++) free(ns->names[i]);
    free(ns->names);
    ns_init(ns);
}

static void ns_add(NameSet *ns, const char *name)
{
    for (int i = 0; i < ns->count; i++)
        if (strcmp(ns->names[i], name) == 0) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 16;
        ns->names = realloc(ns->names, (size_t)ns->cap * sizeof(char *));
    }
    ns->names[ns->count++] = strdup(name);
}

static void ns_free_from(NameSet *ns, int base)
{
    for (int i = base; i < ns->count; i++) free(ns->names[i]);
    ns->count = base;
}

static int ns_contains(const NameSet *ns, const char *name)
{
    for (int i = 0; i < ns->count; i++)
        if (strcmp(ns->names[i], name) == 0) return 1;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void symtab_init(SymTab *t, const char *script)
{
    ns_init(&t->members);
    ns_init(&t->funcs);
    ns_init(&t->engine);
    ns_init(&t->locals);
    fss_init(&t->user_sigs);
    t->locals_base    = 0;
    t->in_body        = 0;
    t->has_engine_api = 0;
    t->errors         = 0;
    t->parent[0]   = '\0';
    t->script      = script;
}

void symtab_free(SymTab *t)
{
    ns_free(&t->members);
    ns_free(&t->funcs);
    ns_free(&t->engine);
    ns_free(&t->locals);
    fss_free(&t->user_sigs);
}

int symtab_load_engine(SymTab *t, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "symtab: cannot open engine-api file '%s'\n", path);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && isspace((unsigned char)line[len-1])) len--;
        line[len] = '\0';
        if (len > 0) ns_add(&t->engine, line);
    }
    fclose(f);
    t->has_engine_api = 1;
    return 0;
}

int symtab_load_inherited(SymTab *t, const char *db_path)
{
    if (t->parent[0] == '\0') return 0;
    FILE *f = fopen(db_path, "r");
    if (!f) {
        fprintf(stderr, "symtab: cannot open member-db '%s'\n", db_path);
        return -1;
    }
    size_t plen  = strlen(t->parent);
    char   line[65536];
    int    found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, t->parent, plen) != 0) continue;
        char c = line[plen];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\0') continue;
        /* parse space-separated member names */
        char *p = line + plen;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\r' || *p == '\0') break;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            char save = *p; *p = '\0';
            ns_add(&t->members, start);
            *p = save;
        }
        found = 1;
        break;
    }
    fclose(f);
    return found ? 0 : -1;
}

/* ── Pre-scan helpers ────────────────────────────────────────────────────── */

/* Count typed parameters in a '(' ... ')' list.  Called with the lexer
 * positioned at '('.  Advances past ')' and returns the count.
 * Each parameter starts with a type keyword (KW_INT..KW_STR). */
static int count_params_lex(Lexer *lex)
{
    if (lex->cur.kind != TOK_LPAREN) return 0;
    lex_advance(lex);  /* consume '(' */
    if (lex->cur.kind == TOK_RPAREN) { lex_advance(lex); return 0; }
    int count = 0, depth = 1;
    for (;;) {
        TokKind k = lex->cur.kind;
        if (k == TOK_EOF) break;
        if (k == TOK_LPAREN)  { depth++; lex_advance(lex); continue; }
        if (k == TOK_RPAREN)  {
            if (--depth == 0) { lex_advance(lex); break; }
            lex_advance(lex); continue;
        }
        if (depth == 1 && k >= KW_INT && k <= KW_STR) count++;
        lex_advance(lex);
    }
    return count;
}

/* ── Pre-scan ────────────────────────────────────────────────────────────── */

void symtab_prescan(SymTab *t, const char *src)
{
    Lexer lex;
    lex_init(&lex, src);

    while (lex.cur.kind != TOK_EOF) {
        if (lex.cur.kind == KW_INHERITS) {
            lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                size_t n = strlen(lex.cur.text);
                if (n >= sizeof(t->parent)) n = sizeof(t->parent) - 1;
                memcpy(t->parent, lex.cur.text, n);
                t->parent[n] = '\0';
            }

        } else if (lex.cur.kind == KW_MEMBER) {
            /* member TYPE NAME */
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT)
                ns_add(&t->members, lex.cur.text);

        } else if (lex.cur.kind == KW_FORWARD) {
            /* forward TYPE NAME(params) */
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                char fname[MAX_TOK_TEXT];
                strncpy(fname, lex.cur.text, sizeof(fname) - 1);
                fname[sizeof(fname)-1] = '\0';
                ns_add(&t->funcs, fname);
                lex_advance(&lex);
                fss_add(&t->user_sigs, fname, count_params_lex(&lex));
            }

        } else if (lex.cur.kind == KW_FUNCTION) {
            /* function [TYPE] NAME(params) { body } — or forward-declare form: ; */
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                char fname[MAX_TOK_TEXT];
                strncpy(fname, lex.cur.text, sizeof(fname) - 1);
                fname[sizeof(fname)-1] = '\0';
                ns_add(&t->funcs, fname);
                lex_advance(&lex);
                int params = count_params_lex(&lex);
                /* Only record signature for full definitions (body follows).
                 * 'function NAME();' is a forward-reference shorthand; the
                 * actual param count is unknown and must come from the definition. */
                if (lex.cur.kind == TOK_LBRACE)
                    fss_add(&t->user_sigs, fname, params);
            }

        } else if (lex.cur.kind == KW_TRIGGER) {
            /* trigger [INT] NAME — register as callable */
            lex_advance(&lex);
            if (lex.cur.kind == TOK_INT) lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT)
                ns_add(&t->funcs, lex.cur.text);

        } else {
            lex_advance(&lex);
        }
    }
}

/* Scan src for member/function declarations and merge into t.
 * Unlike symtab_prescan, does not update t->parent — used for inherited scripts. */
static void prescan_inherit(SymTab *t, const char *src)
{
    Lexer lex;
    lex_init(&lex, src);

    while (lex.cur.kind != TOK_EOF) {
        if (lex.cur.kind == KW_MEMBER) {
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT)
                ns_add(&t->members, lex.cur.text);

        } else if (lex.cur.kind == KW_FORWARD) {
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                char fname[MAX_TOK_TEXT];
                strncpy(fname, lex.cur.text, sizeof(fname) - 1);
                fname[sizeof(fname)-1] = '\0';
                ns_add(&t->funcs, fname);
                lex_advance(&lex);
                fss_add(&t->user_sigs, fname, count_params_lex(&lex));
            }

        } else if (lex.cur.kind == KW_FUNCTION) {
            lex_advance(&lex);
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                char fname[MAX_TOK_TEXT];
                strncpy(fname, lex.cur.text, sizeof(fname) - 1);
                fname[sizeof(fname)-1] = '\0';
                ns_add(&t->funcs, fname);
                lex_advance(&lex);
                int params = count_params_lex(&lex);
                if (lex.cur.kind == TOK_LBRACE)
                    fss_add(&t->user_sigs, fname, params);
            }

        } else if (lex.cur.kind == KW_TRIGGER) {
            lex_advance(&lex);
            if (lex.cur.kind == TOK_INT) lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT)
                ns_add(&t->funcs, lex.cur.text);
        } else {
            lex_advance(&lex);
        }
    }
}

/* Extract the `inherits <name>` parent name from src into buf (size bufsz).
 * Returns 1 if found, 0 if not. */
static int extract_parent(const char *src, char *buf, size_t bufsz)
{
    Lexer lex;
    lex_init(&lex, src);
    while (lex.cur.kind != TOK_EOF) {
        if (lex.cur.kind == KW_INHERITS) {
            lex_advance(&lex);
            if (lex.cur.kind == TOK_IDENT) {
                size_t n = strlen(lex.cur.text);
                if (n >= bufsz) n = bufsz - 1;
                memcpy(buf, lex.cur.text, n);
                buf[n] = '\0';
                return 1;
            }
            return 0;
        }
        lex_advance(&lex);
    }
    return 0;
}

/* Load members/functions from a parent script file in scripts_dir,
 * then recursively follow that parent's own inherits chain (up to depth 16). */
int symtab_load_parent_from_dir(SymTab *t, const char *scripts_dir, const char *parent_name, int depth)
{
    if (depth <= 0 || !parent_name || parent_name[0] == '\0') return 0;

    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s.m", scripts_dir, parent_name);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); return -1; }
    sz = (long)fread(src, 1, (size_t)sz, f);
    src[sz] = '\0';
    fclose(f);

    prescan_inherit(t, src);

    char grand[256] = "";
    if (extract_parent(src, grand, sizeof(grand)) && strcmp(grand, parent_name) != 0)
        symtab_load_parent_from_dir(t, scripts_dir, grand, depth - 1);

    free(src);
    return 0;
}

/* ── Scope management ────────────────────────────────────────────────────── */

void symtab_push_scope(SymTab *t)
{
    t->locals_base = t->locals.count;
    t->in_body     = 1;
}

void symtab_pop_scope(SymTab *t)
{
    ns_free_from(&t->locals, t->locals_base);
    t->locals_base = 0;
    if (t->locals.count == 0) t->in_body = 0;
}

void symtab_add_local(SymTab *t, const char *name)
{
    if (t) ns_add(&t->locals, name);
}

/* ── Symbol check ────────────────────────────────────────────────────────── */

void symtab_check(SymTab *t, const char *name, int line, int is_call)
{
    /* 'this' is always valid — refers to the item the script is attached to. */
    if (strcmp(name, "this") == 0) return;

    /* Locals and params are always valid (call or non-call). */
    if (ns_contains(&t->locals,  name)) return;

    /* Module member variables are always valid. */
    if (ns_contains(&t->members, name)) return;

    /* Functions declared in this script are valid as calls or bare refs. */
    if (ns_contains(&t->funcs,   name)) return;

    /* Implicit trigger variables injected by the runtime. */
    for (int i = 0; IMPLICIT_VARS[i]; i++)
        if (strcmp(IMPLICIT_VARS[i], name) == 0) return;

    if (is_call) {
        /* When engine-api is loaded, validate against it; otherwise we cannot
         * know which names the engine provides, so accept all call-site uses. */
        if (!t->has_engine_api) return;
        if (ns_contains(&t->engine, name)) return;
    }
    /* Variable reference: do NOT accept engine/global function names —
     * you cannot use a function as a first-class value in Wombat. */

    fprintf(stderr, "%s:%d: undefined symbol '%s'\n", t->script, line, name);
    t->errors++;
}

void symtab_check_call_argc(SymTab *t, const char *name, int line, int argc)
{
    int expected = fss_lookup(&t->user_sigs, name);
    if (expected < 0) return;
    if (argc != expected) {
        fprintf(stderr, "%s:%d: '%s' expects %d argument(s), got %d\n",
                t->script, line, name, expected, argc);
        t->errors++;
    }
}
