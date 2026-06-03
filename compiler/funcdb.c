#include "funcdb.h"
#include "lex.h"
#include "tokens.h"
#include <stdio.h>
#include <string.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int is_type_kw(TokKind k)
{
    switch (k) {
        case KW_INT: case KW_STRING: case KW_USTRING: case KW_LOC:
        case KW_OBJ: case KW_LIST:  case KW_VOID:    case KW_FLOAT:
        case KW_STR:
            return 1;
        default:
            return 0;
    }
}

static FuncDef *find_or_add(FuncDB *db, const char *name)
{
    for (int i = 0; i < db->n_defs; i++) {
        if (strcmp(db->defs[i].name, name) == 0) return &db->defs[i];
    }
    if (db->n_defs >= FUNCDB_MAX_FUNCS) return NULL;
    FuncDef *fd = &db->defs[db->n_defs++];
    memset(fd, 0, sizeof(*fd));
    strncpy(fd->name, name, FUNCDB_NAME_LEN - 1);
    return fd;
}

static int find_param(const FuncDef *fd, const char *name)
{
    for (int i = 0; i < fd->n_params; i++) {
        if (strcmp(fd->params[i], name) == 0) return i;
    }
    return -1;
}

/* ── body scan ─────────────────────────────────────────────────────────────── */

/* Scan function body recording param-forwarding triples.
 * Called after the opening '{' has been consumed; advances past closing '}'. */
static void scan_body(FuncDef *fd, Lexer *lex)
{
    /* Local call-frame stack — tracks active function calls. */
    struct {
        char callee[FUNCDB_NAME_LEN];
        int  argidx;
        int  depth;   /* paren_depth at which this call was opened */
    } frames[FUNCDB_CALL_DEPTH];

    int n_frames      = 0;
    int paren_depth   = 0;
    int brace_depth   = 1;          /* one brace already consumed by caller */
    int last_was_delim = 0;         /* last token was '(' opening a call or ',' */
    char prev_ident[FUNCDB_NAME_LEN] = "";

    while (lex->cur.kind != TOK_EOF) {
        TokKind k = lex->cur.kind;

        /* Flush deferred IDENT: process it as a bare (non-call) identifier.
         * We defer because we can't tell IDENT-as-call from IDENT-as-expr
         * until we see whether the next token is '('. */
        if (prev_ident[0] && k != TOK_LPAREN) {
            /* Record a param-forward if: we're in a call's argument slot, the
             * last delimiter was '(' or ',', and the name matches a parameter.
             * Skip member-access chains (k == TOK_DOT means "a.b" — a is not
             * a direct argument). */
            if (k != TOK_DOT && last_was_delim &&
                n_frames > 0 && frames[n_frames-1].depth == paren_depth) {
                int pidx = find_param(fd, prev_ident);
                if (pidx >= 0 && fd->n_forwards < FUNCDB_MAX_FORWARDS) {
                    ParamForward *pf = &fd->forwards[fd->n_forwards++];
                    strncpy(pf->callee, frames[n_frames-1].callee,
                            FUNCDB_NAME_LEN - 1);
                    pf->callee[FUNCDB_NAME_LEN - 1] = '\0';
                    pf->callee_arg = frames[n_frames-1].argidx;
                    pf->my_param   = pidx;
                }
            }
            last_was_delim = 0;
            prev_ident[0]  = '\0';
        }

        if (k == TOK_IDENT) {
            strncpy(prev_ident, lex->cur.text, FUNCDB_NAME_LEN - 1);
            prev_ident[FUNCDB_NAME_LEN - 1] = '\0';

        } else if (k == TOK_LPAREN) {
            paren_depth++;
            if (prev_ident[0]) {
                /* IDENT immediately before '(' — this is a function call. */
                if (n_frames < FUNCDB_CALL_DEPTH) {
                    strncpy(frames[n_frames].callee, prev_ident,
                            FUNCDB_NAME_LEN - 1);
                    frames[n_frames].callee[FUNCDB_NAME_LEN - 1] = '\0';
                    frames[n_frames].argidx = 0;
                    frames[n_frames].depth  = paren_depth;
                    n_frames++;
                }
                last_was_delim = 1;
                prev_ident[0]  = '\0';
            } else {
                last_was_delim = 0;
            }

        } else if (k == TOK_RPAREN) {
            /* Pop call frame before decrementing depth. */
            if (n_frames > 0 && frames[n_frames-1].depth == paren_depth)
                n_frames--;
            paren_depth--;
            last_was_delim = 0;

        } else if (k == TOK_COMMA) {
            if (n_frames > 0 && frames[n_frames-1].depth == paren_depth) {
                frames[n_frames-1].argidx++;
                last_was_delim = 1;
            } else {
                last_was_delim = 0;
            }

        } else if (k == TOK_LBRACE) {
            brace_depth++;
            last_was_delim = 0;

        } else if (k == TOK_RBRACE) {
            brace_depth--;
            if (brace_depth == 0) {
                lex_advance(lex);   /* consume the closing '}' */
                return;
            }
            last_was_delim = 0;
        }

        lex_advance(lex);
    }
}

/* ── public API ────────────────────────────────────────────────────────────── */

void funcdb_prescan(FuncDB *db, const char *src)
{
    Lexer lex;
    lex_init(&lex, src); /* lex_init internally advances to the first token */

    while (lex.cur.kind != TOK_EOF) {
        TokKind k = lex.cur.kind;

        if (k == KW_FUNCTION || k == KW_TRIGGER || k == KW_MEMBER) {
            lex_advance(&lex);
            /* Skip return-type / qualifier keywords that precede the name.
             * KW_INT through KW_FORWARD is the contiguous keyword range. */
            while (lex.cur.kind >= KW_INT && lex.cur.kind <= KW_FORWARD)
                lex_advance(&lex);
            /* Triggers may have an integer event id before the name. */
            if (k == KW_TRIGGER && lex.cur.kind == TOK_INT)
                lex_advance(&lex);
            if (lex.cur.kind != TOK_IDENT) continue;

            char fname[FUNCDB_NAME_LEN];
            strncpy(fname, lex.cur.text, FUNCDB_NAME_LEN - 1);
            fname[FUNCDB_NAME_LEN - 1] = '\0';

            FuncDef *fd = find_or_add(db, fname);
            if (!fd) { lex_advance(&lex); continue; }

            lex_advance(&lex); /* past function name */

            /* Collect parameter names from the parameter list. */
            if (lex.cur.kind == TOK_LPAREN) {
                lex_advance(&lex); /* past '(' */
                while (lex.cur.kind != TOK_RPAREN && lex.cur.kind != TOK_EOF) {
                    if (is_type_kw(lex.cur.kind)) {
                        lex_advance(&lex); /* past type keyword */
                        if (lex.cur.kind == TOK_IDENT &&
                            fd->n_params < FUNCDB_MAX_PARAMS) {
                            strncpy(fd->params[fd->n_params], lex.cur.text,
                                    FUNCDB_NAME_LEN - 1);
                            fd->params[fd->n_params][FUNCDB_NAME_LEN - 1] = '\0';
                            fd->n_params++;
                        }
                        if (lex.cur.kind != TOK_EOF) lex_advance(&lex);
                    } else {
                        lex_advance(&lex);
                    }
                }
                if (lex.cur.kind == TOK_RPAREN) lex_advance(&lex);
            }

            /* Advance to opening brace; stop early on forward declarations. */
            while (lex.cur.kind != TOK_LBRACE && lex.cur.kind != TOK_EOF &&
                   lex.cur.kind != TOK_SEMI   &&
                   lex.cur.kind != KW_FUNCTION &&
                   lex.cur.kind != KW_TRIGGER  &&
                   lex.cur.kind != KW_MEMBER) {
                lex_advance(&lex);
            }

            if (lex.cur.kind == TOK_LBRACE) {
                lex_advance(&lex); /* consume '{' */
                scan_body(fd, &lex);
            }
            /* else: forward declaration — cursor already at ';' or next kw;
             * continue outer loop without extra advance */
            continue;
        }

        lex_advance(&lex);
    }
}

int funcdb_propagate(FuncDB *db, EnumDB *enumdb)
{
    int total = 0;
    int changed;

    do {
        changed = 0;
        for (int i = 0; i < db->n_defs; i++) {
            FuncDef *fd = &db->defs[i];
            for (int j = 0; j < fd->n_forwards; j++) {
                ParamForward *pf = &fd->forwards[j];
                const char *type =
                    enumdb_get_annotation(enumdb, pf->callee, pf->callee_arg);
                if (!type) continue;
                if (enumdb_get_annotation(enumdb, fd->name, pf->my_param))
                    continue;
                enumdb_add_annot(enumdb, fd->name, pf->my_param, type);
                total++;
                changed++;
            }
        }
    } while (changed);

    return total;
}

void funcdb_print_annots(const EnumDB *enumdb, FILE *f)
{
    for (int i = enumdb->n_annots_file; i < enumdb->n_annots; i++) {
        const EnumAnnotation *a = &enumdb->annots[i];
        fprintf(f, "%-35s %d %s\n", a->func_name, a->arg_index, a->type_name);
    }
}
