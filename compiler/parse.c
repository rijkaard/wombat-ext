#include "tokens.h"
#include "sdb.h"
#include "emit.h"
#include "lex.h"
#include "symtab.h"
#include "enumdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Parser state ──────────────────────────────────────────────────────────── */

typedef struct {
    Lexer  *lex;
    Emit   *emit;
    SDB    *sdb;
    SymTab *symtab; /* NULL when symbol checking is disabled */
    EnumDB *enumdb; /* NULL when enum support is disabled */
    /* call-context stack for enum type validation */
    char   ctx_func[ENUMDB_CTX_DEPTH][ENUMDB_CTX_FUNC_LEN];
    int    ctx_argidx[ENUMDB_CTX_DEPTH];
    int    call_depth;
} Parser;

/* ── Diagnostics ───────────────────────────────────────────────────────────── */

static void parse_err(Parser *p, const char *msg)
{
    fprintf(stderr, "parse error (line %d): %s (got token kind=%d text='%s')\n",
            p->lex->cur.line, msg, p->lex->cur.kind, p->lex->cur.text);
    exit(1);
}

/* ── Token helpers ─────────────────────────────────────────────────────────── */

static void advance(Parser *p) { lex_advance(p->lex); }

static void expect(Parser *p, TokKind k)
{
    if (p->lex->cur.kind != k) {
        char msg[256];
        snprintf(msg, sizeof(msg), "expected token kind %d", (int)k);
        parse_err(p, msg);
    }
    advance(p);
}

static int match(Parser *p, TokKind k)
{
    if (p->lex->cur.kind == k) { advance(p); return 1; }
    return 0;
}

/* ── Type-keyword helpers ──────────────────────────────────────────────────── */

static int is_type_kw(TokKind k)
{
    switch (k) {
    case KW_INT: case KW_STRING: case KW_USTRING: case KW_LOC:
    case KW_OBJ: case KW_LIST:  case KW_VOID:
    case KW_FLOAT: case KW_STR:
        return 1;
    default: return 0;
    }
}

static uint16_t type_kw_to_bin(TokKind k)
{
    switch (k) {
    case KW_INT:     return BIN_TK_INT;
    case KW_STRING:  return BIN_TK_STRING;
    case KW_USTRING: return BIN_TK_USTRING;
    case KW_STR:     return BIN_TK_STRING;
    case KW_LOC:     return BIN_TK_LOC;
    case KW_OBJ:     return BIN_TK_OBJ;
    case KW_LIST:    return BIN_TK_LIST;
    case KW_VOID:    return BIN_TK_VOID;
    case KW_FLOAT:   return BIN_TK_INT; /* best effort */
    default:         return BIN_TK_INT;
    }
}

/* Emit the type keyword token and consume it. */
static void parse_type(Parser *p)
{
    emit_tok(p->emit, type_kw_to_bin(p->lex->cur.kind));
    advance(p);
}

/* ── Binary-operator helpers ───────────────────────────────────────────────── */

static int is_binop(TokKind k)
{
    switch (k) {
    case TOK_ADD: case TOK_SUB: case TOK_MUL: case TOK_DIV: case TOK_MOD:
    case TOK_EQ:  case TOK_NEQ:
    case TOK_LT:  case TOK_GT: case TOK_LTEQ: case TOK_GTEQ:
    case TOK_AND: case TOK_OR: case TOK_XOR:
        return 1;
    default: return 0;
    }
}

static uint16_t binop_to_bin(TokKind k)
{
    switch (k) {
    case TOK_ADD:  return BIN_OP_ADD;
    case TOK_SUB:  return BIN_OP_SUB;
    case TOK_MUL:  return BIN_OP_MUL;
    case TOK_DIV:  return BIN_OP_DIV;
    case TOK_MOD:  return BIN_OP_MOD;
    case TOK_EQ:   return BIN_OP_ISEQ;
    case TOK_NEQ:  return BIN_OP_ISNEQ;
    case TOK_LT:   return BIN_OP_LT;
    case TOK_GT:   return BIN_OP_GT;
    case TOK_LTEQ: return BIN_OP_LTEQ;
    case TOK_GTEQ: return BIN_OP_GTEQ;
    case TOK_AND:  return BIN_OP_LOGAND;
    case TOK_OR:   return BIN_OP_LOGOR;
    case TOK_XOR:  return BIN_OP_XOR;
    default:       return 0;
    }
}

/* ── Forward declarations ──────────────────────────────────────────────────── */

static void parse_expr(Parser *p);
static void parse_block(Parser *p);
static void parse_stmt(Parser *p);

/* Returns 1 if token k can only start a new statement, not continue current one. */
static int is_stmt_start(TokKind k)
{
    switch (k) {
    case TOK_IDENT: case TOK_RBRACE: case TOK_EOF:
    case KW_IF: case KW_ELSE: case KW_WHILE: case KW_FOR: case KW_SWITCH:
    case KW_RETURN: case KW_BREAK: case KW_CONTINUE: case KW_GOTO:
    case KW_FUNCTION: case KW_TRIGGER: case KW_MEMBER: case KW_FORWARD:
    case KW_INHERITS:
    case KW_INT: case KW_STRING: case KW_USTRING: case KW_LOC:
    case KW_OBJ: case KW_LIST: case KW_VOID: case KW_FLOAT: case KW_STR:
        return 1;
    default:
        return 0;
    }
}

/* Consume ';' if present; tolerate its absence at statement boundaries.
 * Only emits SM_SEMI when the ';' was actually in the source — the original
 * compiler skips the token entirely when ';' is absent. */
static void soft_semi(Parser *p)
{
    if (p->lex->cur.kind == TOK_SEMI) {
        advance(p);
        emit_tok(p->emit, BIN_SM_SEMI);
    } else if (!is_stmt_start(p->lex->cur.kind)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected ';' (got kind %d)", (int)p->lex->cur.kind);
        parse_err(p, msg);
    }
    /* else: ';' absent at statement boundary — emit nothing */
}

/* ── Expression parser ─────────────────────────────────────────────────────── */

static int parse_arg_list(Parser *p)
{
    /* Emits comma-separated expressions. Does NOT emit surrounding parens.
     * Returns the number of arguments parsed. */
    if (p->lex->cur.kind == TOK_RPAREN) return 0;
    int argc = 1;
    parse_expr(p);
    for (;;) {
        if (p->lex->cur.kind == TOK_COMMA) {
            advance(p);
            emit_tok(p->emit, BIN_SM_COMMA);
            if (p->enumdb && p->call_depth > 0)
                p->ctx_argidx[p->call_depth - 1]++;
            if (p->lex->cur.kind == TOK_RPAREN) break; /* trailing comma */
            argc++;
            parse_expr(p);
        } else if (p->lex->cur.kind == TOK_STRING || p->lex->cur.kind == TOK_USTRING
                || p->lex->cur.kind == TOK_INT) {
            /* adjacent literal without comma — no SM_COMMA emitted */
            if (p->enumdb && p->call_depth > 0)
                p->ctx_argidx[p->call_depth - 1]++;
            argc++;
            parse_expr(p);
        } else {
            break;
        }
    }
    return argc;
}

static void parse_expr_primary(Parser *p)
{
    TokKind k = p->lex->cur.kind;

    if (k == TOK_NULL_LIT) {
        emit_id(p->emit, p->sdb, "NULL");
        emit_tok(p->emit, BIN_SM_LPAREN);
        emit_tok(p->emit, BIN_SM_RPAREN);
        advance(p);
        return;
    }

    if (k == TOK_IDENT || (k >= KW_INT && k <= KW_FORWARD)) {
        /* keywords used as identifiers (e.g. 'default') fall through here */
        char name[MAX_TOK_TEXT];
        int  line = p->lex->cur.line;
        strncpy(name, p->lex->cur.text, MAX_TOK_TEXT);
        name[MAX_TOK_TEXT-1] = '\0';
        advance(p);

        int is_call = (p->lex->cur.kind == TOK_LPAREN);

        /* Enum constant resolution: plain identifier, not a function call. */
        if (!is_call && p->enumdb && k == TOK_IDENT) {
            int64_t     eval;
            const char *etype;
            if (enumdb_lookup(p->enumdb, name, &eval, &etype)) {
                /* Type-check against the enclosing call's expected type */
                if (p->call_depth > 0) {
                    const char *expected = enumdb_get_annotation(
                        p->enumdb,
                        p->ctx_func[p->call_depth - 1],
                        p->ctx_argidx[p->call_depth - 1]);
                    if (expected && strcmp(expected, etype) != 0) {
                        fprintf(stderr,
                            "error (line %d): enum '%s' has type '%s' but"
                            " '%s' arg %d expects '%s'\n",
                            line, name, etype,
                            p->ctx_func[p->call_depth - 1],
                            p->ctx_argidx[p->call_depth - 1],
                            expected);
                        exit(1);
                    }
                }
                emit_int(p->emit, eval, 0);
                return;
            }
        }

        emit_id(p->emit, p->sdb, name);

        /* Only check plain TOK_IDENT — keyword-as-identifier tokens are valid
         * by definition (e.g. 'default' used as a variable). */
        if (p->symtab && k == TOK_IDENT)
            symtab_check(p->symtab, name, line, is_call);

        if (is_call) {
            advance(p);
            emit_tok(p->emit, BIN_SM_LPAREN);

            /* Push call context for enum type validation */
            if (p->enumdb && p->call_depth < ENUMDB_CTX_DEPTH) {
                strncpy(p->ctx_func[p->call_depth], name,
                        ENUMDB_CTX_FUNC_LEN - 1);
                p->ctx_func[p->call_depth][ENUMDB_CTX_FUNC_LEN - 1] = '\0';
                p->ctx_argidx[p->call_depth] = 0;
                p->call_depth++;
            }

            int argc = parse_arg_list(p);

            if (p->enumdb && p->call_depth > 0) p->call_depth--;

            expect(p, TOK_RPAREN);
            emit_tok(p->emit, BIN_SM_RPAREN);
            if (p->symtab && k == TOK_IDENT)
                symtab_check_call_argc(p->symtab, name, line, argc);
        }
        return;
    }

    if (k == TOK_INT) {
        int hd = p->lex->cur.ihexdigits;
        emit_int(p->emit, p->lex->cur.ival, hd);
        advance(p);
        return;
    }

    if (k == TOK_STRING) {
        emit_str(p->emit, p->sdb, p->lex->cur.text);
        advance(p);
        return;
    }

    if (k == TOK_USTRING) {
        emit_ustr(p->emit, p->sdb, p->lex->cur.text);
        advance(p);
        return;
    }

    if (k == TOK_LPAREN) {
        emit_tok(p->emit, BIN_SM_LPAREN);
        advance(p);
        parse_expr(p);
        emit_tok(p->emit, BIN_SM_RPAREN);
        expect(p, TOK_RPAREN);
        return;
    }

    parse_err(p, "expected expression");
}

static void parse_expr_postfix(Parser *p)
{
    parse_expr_primary(p);

    for (;;) {
        if (p->lex->cur.kind == TOK_INC) {
            emit_tok(p->emit, BIN_OP_INC);
            advance(p);
        } else if (p->lex->cur.kind == TOK_DEC) {
            emit_tok(p->emit, BIN_OP_DEC);
            advance(p);
        } else if (p->lex->cur.kind == TOK_LBRACKET) {
            advance(p);
            emit_tok(p->emit, BIN_SM_LBRACKET);
            parse_expr(p);
            expect(p, TOK_RBRACKET);
            emit_tok(p->emit, BIN_SM_RBRACKET);
        } else {
            break;
        }
    }
}

static void parse_expr_unary(Parser *p)
{
    if (p->lex->cur.kind == TOK_NOT) {
        advance(p);
        emit_tok(p->emit, BIN_OP_NOT);
        parse_expr_unary(p);
        return;
    }
    if (p->lex->cur.kind == TOK_SUB) {
        /* Unary minus: emit as 0 - expr */
        advance(p);
        emit_int(p->emit, 0, 0);
        emit_tok(p->emit, BIN_OP_SUB);
        parse_expr_unary(p);
        return;
    }
    parse_expr_postfix(p);
}

static void parse_expr(Parser *p)
{
    parse_expr_unary(p);
    while (is_binop(p->lex->cur.kind)) {
        uint16_t op = binop_to_bin(p->lex->cur.kind);
        advance(p);
        emit_tok(p->emit, op);
        parse_expr_unary(p);
    }
}

/* ── Block ─────────────────────────────────────────────────────────────────── */

static void parse_block(Parser *p)
{
    expect(p, TOK_LBRACE);
    emit_tok(p->emit, BIN_SM_LBRACE);
    while (p->lex->cur.kind != TOK_RBRACE && p->lex->cur.kind != TOK_EOF)
        parse_stmt(p);
    expect(p, TOK_RBRACE);
    emit_tok(p->emit, BIN_SM_RBRACE);
}

/* ── Statements ────────────────────────────────────────────────────────────── */

/*
 * parse_var_decl — parse "type IDENT ['=' initExpr] ';'"
 *
 * is_member: non-zero when called from parse_member_decl (top-level member).
 *            The name must NOT be registered as a local in that case.
 */
static void parse_var_decl(Parser *p, int is_member)
{
    parse_type(p);

    char name[MAX_TOK_TEXT];
    int  line = p->lex->cur.line;
    /* allow keywords as variable names (e.g. 'default') */
    if (p->lex->cur.kind != TOK_IDENT
        && !(p->lex->cur.kind >= KW_INT && p->lex->cur.kind <= KW_FORWARD))
        parse_err(p, "expected variable name");
    strncpy(name, p->lex->cur.text, MAX_TOK_TEXT);
    name[MAX_TOK_TEXT-1] = '\0';
    advance(p);
    emit_id(p->emit, p->sdb, name);

    /* Register or check */
    if (!is_member && p->symtab) {
        /* Local variable declaration: register so later uses are valid */
        symtab_add_local(p->symtab, name);
    }
    /* For member declarations the name was already added by prescan; no check
     * needed — declaration is always valid. */
    (void)line;

    if (p->lex->cur.kind == TOK_ASSIGN || p->lex->cur.kind == TOK_SUB) {
        /* Some sources use '-' instead of '=' (encode as-is) */
        uint16_t op = (p->lex->cur.kind == TOK_ASSIGN) ? BIN_OP_ASSIGN : BIN_OP_SUB;
        advance(p);
        emit_tok(p->emit, op);
        /* varInit: comma-or-adjacent-separated exprs */
        parse_expr(p);
        for (;;) {
            if (p->lex->cur.kind == TOK_COMMA) {
                advance(p);
                emit_tok(p->emit, BIN_SM_COMMA);
                parse_expr(p);
            } else if (p->lex->cur.kind == TOK_INT || p->lex->cur.kind == TOK_STRING
                    || p->lex->cur.kind == TOK_USTRING) {
                /* adjacent literal without comma — no SM_COMMA */
                parse_expr(p);
            } else {
                break;
            }
        }
    }

    /* emit stray closing parens before semicolon */
    while (p->lex->cur.kind == TOK_RPAREN) {
        emit_tok(p->emit, BIN_SM_RPAREN);
        advance(p);
    }
    soft_semi(p);
}

static void parse_member_decl(Parser *p)
{
    /* 'member' type IDENT ['=' expr] ';' */
    expect(p, KW_MEMBER);
    emit_tok(p->emit, BIN_TK_MEMBER);
    parse_var_decl(p, 1 /* is_member */);
}

static void parse_if(Parser *p)
{
    expect(p, KW_IF);
    emit_tok(p->emit, BIN_TK_IF);
    emit_tok(p->emit, BIN_SM_LPAREN);
    expect(p, TOK_LPAREN);
    parse_expr(p);
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);
    parse_block(p);

    if (p->lex->cur.kind == KW_ELSE) {
        advance(p);
        emit_tok(p->emit, BIN_TK_ELSE);
        if (p->lex->cur.kind == KW_IF) {
            parse_if(p); /* else if — no wrapper braces */
        } else {
            parse_block(p);
        }
    }
}

static void parse_while(Parser *p)
{
    expect(p, KW_WHILE);
    emit_tok(p->emit, BIN_TK_WHILE);
    emit_tok(p->emit, BIN_SM_LPAREN);
    expect(p, TOK_LPAREN);
    parse_expr(p);
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);
    parse_block(p);
}

/*
 * for (forInit; cond; forUpdate) block
 *
 * Binary: TK_FOR SM_LPAREN [type] T_ID OP_ASSIGN expr SM_SEMI
 *                          cond SM_SEMI
 *                          T_ID OP_INC/DEC SM_RPAREN block
 */
static void parse_for(Parser *p)
{
    expect(p, KW_FOR);
    emit_tok(p->emit, BIN_TK_FOR);
    emit_tok(p->emit, BIN_SM_LPAREN);
    expect(p, TOK_LPAREN);

    /* forInit */
    if (p->lex->cur.kind != TOK_SEMI) {
        int for_decl = 0;
        if (is_type_kw(p->lex->cur.kind)) {
            parse_type(p);
            for_decl = 1;
        }
        /* IDENT = expr */
        if (p->lex->cur.kind != TOK_IDENT)
            parse_err(p, "expected identifier in for-init");
        char for_name[MAX_TOK_TEXT];
        int  for_line = p->lex->cur.line;
        strncpy(for_name, p->lex->cur.text, MAX_TOK_TEXT);
        for_name[MAX_TOK_TEXT-1] = '\0';
        emit_id(p->emit, p->sdb, for_name);
        advance(p);
        if (for_decl) {
            /* New local — register, don't check */
            if (p->symtab) symtab_add_local(p->symtab, for_name);
        } else {
            /* Existing variable — check */
            if (p->symtab) symtab_check(p->symtab, for_name, for_line, 0);
        }
        if (p->lex->cur.kind == TOK_ASSIGN) {
            advance(p);
            emit_tok(p->emit, BIN_OP_ASSIGN);
            parse_expr(p);
        }
    }
    expect(p, TOK_SEMI);
    emit_tok(p->emit, BIN_SM_SEMI);

    /* condition */
    if (p->lex->cur.kind != TOK_SEMI) {
        parse_expr(p);
    }
    expect(p, TOK_SEMI);
    emit_tok(p->emit, BIN_SM_SEMI);

    /* forUpdate */
    if (p->lex->cur.kind != TOK_RPAREN) {
        if (p->lex->cur.kind == TOK_IDENT) {
            int up_line = p->lex->cur.line;
            if (p->symtab) symtab_check(p->symtab, p->lex->cur.text, up_line, 0);
            emit_id(p->emit, p->sdb, p->lex->cur.text);
            advance(p);
        }
        if (p->lex->cur.kind == TOK_INC) {
            emit_tok(p->emit, BIN_OP_INC);
            advance(p);
        } else if (p->lex->cur.kind == TOK_DEC) {
            emit_tok(p->emit, BIN_OP_DEC);
            advance(p);
        } else if (p->lex->cur.kind == TOK_ASSIGN
                || p->lex->cur.kind == TOK_ADD_ASSIGN
                || p->lex->cur.kind == TOK_SUB_ASSIGN) {
            /* bare assignment update */
            emit_tok(p->emit, BIN_OP_ASSIGN);
            advance(p);
            parse_expr(p);
        }
    }
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);
    parse_block(p);
}

static void parse_switch(Parser *p)
{
    expect(p, KW_SWITCH);
    emit_tok(p->emit, BIN_TK_SWITCH);
    emit_tok(p->emit, BIN_SM_LPAREN);
    expect(p, TOK_LPAREN);
    parse_expr(p);
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);
    expect(p, TOK_LBRACE);
    emit_tok(p->emit, BIN_SM_LBRACE);

    while (p->lex->cur.kind == KW_CASE || p->lex->cur.kind == KW_DEFAULT) {
        /* one or more switchLabels */
        while (p->lex->cur.kind == KW_CASE || p->lex->cur.kind == KW_DEFAULT) {
            if (p->lex->cur.kind == KW_CASE) {
                advance(p);
                emit_tok(p->emit, BIN_TK_CASE);
                parse_expr(p);
                match(p, TOK_COLON);
            } else {
                advance(p);
                emit_tok(p->emit, BIN_TK_DEFAULT);
                match(p, TOK_COLON);
            }
        }
        /* body statements until next case/default/rbrace/eof */
        while (p->lex->cur.kind != KW_CASE
            && p->lex->cur.kind != KW_DEFAULT
            && p->lex->cur.kind != TOK_RBRACE
            && p->lex->cur.kind != TOK_EOF)
        {
            parse_stmt(p);
        }
    }

    expect(p, TOK_RBRACE);
    emit_tok(p->emit, BIN_SM_RBRACE);
}

static void parse_return(Parser *p)
{
    expect(p, KW_RETURN);
    emit_tok(p->emit, BIN_TK_RETURN);

    if (p->lex->cur.kind == TOK_LPAREN) {
        /* return(expr); or return(); */
        advance(p);
        emit_tok(p->emit, BIN_SM_LPAREN);
        if (p->lex->cur.kind != TOK_RPAREN)
            parse_expr(p);
        expect(p, TOK_RPAREN);
        emit_tok(p->emit, BIN_SM_RPAREN);
        soft_semi(p);
    } else {
        /* bare return; — no parens in binary */
        soft_semi(p);
    }
}

/* Emit an expression-or-assignment statement. */
static void parse_expr_stmt(Parser *p)
{
    TokKind k = p->lex->cur.kind;

    if (k == TOK_IDENT) {
        char name[MAX_TOK_TEXT];
        int  line = p->lex->cur.line;
        strncpy(name, p->lex->cur.text, MAX_TOK_TEXT);
        name[MAX_TOK_TEXT-1] = '\0';
        advance(p);

        TokKind next = p->lex->cur.kind;

        if (next == TOK_LPAREN) {
            /* function call statement */
            if (p->symtab) symtab_check(p->symtab, name, line, 1);
            emit_id(p->emit, p->sdb, name);
            advance(p);
            emit_tok(p->emit, BIN_SM_LPAREN);

            /* Push call context for enum type validation */
            if (p->enumdb && p->call_depth < ENUMDB_CTX_DEPTH) {
                strncpy(p->ctx_func[p->call_depth], name,
                        ENUMDB_CTX_FUNC_LEN - 1);
                p->ctx_func[p->call_depth][ENUMDB_CTX_FUNC_LEN - 1] = '\0';
                p->ctx_argidx[p->call_depth] = 0;
                p->call_depth++;
            }

            int argc = parse_arg_list(p);

            if (p->enumdb && p->call_depth > 0) p->call_depth--;

            expect(p, TOK_RPAREN);
            emit_tok(p->emit, BIN_SM_RPAREN);
            if (p->symtab) symtab_check_call_argc(p->symtab, name, line, argc);
            soft_semi(p);

        } else if (next == TOK_ASSIGN || next == TOK_ADD_ASSIGN
                || next == TOK_SUB_ASSIGN || next == TOK_MUL_ASSIGN
                || next == TOK_DIV_ASSIGN) {
            /* simple assignment */
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            emit_tok(p->emit, BIN_OP_ASSIGN);
            advance(p);
            /* For compound ops, wrap as assign of (lhs op rhs) */
            if (next != TOK_ASSIGN) {
                emit_id(p->emit, p->sdb, name);
                uint16_t op = 0;
                switch (next) {
                case TOK_ADD_ASSIGN: op = BIN_OP_ADD; break;
                case TOK_SUB_ASSIGN: op = BIN_OP_SUB; break;
                case TOK_MUL_ASSIGN: op = BIN_OP_MUL; break;
                case TOK_DIV_ASSIGN: op = BIN_OP_DIV; break;
                default: break;
                }
                emit_tok(p->emit, op);
            }
            parse_expr(p);
            /* multi-value list assignment: name = expr, expr, ...; */
            for (;;) {
                if (p->lex->cur.kind == TOK_COMMA) {
                    advance(p);
                    emit_tok(p->emit, BIN_SM_COMMA);
                    parse_expr(p);
                } else if (p->lex->cur.kind == TOK_INT || p->lex->cur.kind == TOK_STRING
                        || p->lex->cur.kind == TOK_USTRING) {
                    /* adjacent literal without comma — no SM_COMMA */
                    parse_expr(p);
                } else {
                    break;
                }
            }
            /* emit stray closing parens before semicolon */
            while (p->lex->cur.kind == TOK_RPAREN) {
                emit_tok(p->emit, BIN_SM_RPAREN);
                advance(p);
            }
            soft_semi(p);

        } else if (next == TOK_INC) {
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            emit_tok(p->emit, BIN_OP_INC);
            advance(p);
            expect(p, TOK_SEMI);
            emit_tok(p->emit, BIN_SM_SEMI);

        } else if (next == TOK_DEC) {
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            emit_tok(p->emit, BIN_OP_DEC);
            advance(p);
            expect(p, TOK_SEMI);
            emit_tok(p->emit, BIN_SM_SEMI);

        } else if (next == TOK_LBRACKET) {
            /* arr[idx] = expr; */
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            advance(p);
            emit_tok(p->emit, BIN_SM_LBRACKET);
            parse_expr(p);
            expect(p, TOK_RBRACKET);
            emit_tok(p->emit, BIN_SM_RBRACKET);
            if (p->lex->cur.kind == TOK_ASSIGN
             || p->lex->cur.kind == TOK_ADD_ASSIGN
             || p->lex->cur.kind == TOK_SUB_ASSIGN) {
                emit_tok(p->emit, BIN_OP_ASSIGN);
                advance(p);
                parse_expr(p);
            }
            expect(p, TOK_SEMI);
            emit_tok(p->emit, BIN_SM_SEMI);

        } else if (next == TOK_DOT) {
            /* obj.field = expr; — member access assignment */
            /* `name` is the object variable — check it */
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            advance(p); /* consume . */
            if (p->lex->cur.kind != TOK_IDENT)
                parse_err(p, "expected field name after '.'");
            /* Encode member access as T_ID.T_ID pair — server-specific */
            emit_tok(p->emit, BIN_SM_LBRACKET);
            emit_str(p->emit, p->sdb, p->lex->cur.text);
            /* field name is emitted as a string literal — no symbol check */
            emit_tok(p->emit, BIN_SM_RBRACKET);
            advance(p);
            if (p->lex->cur.kind == TOK_ASSIGN) {
                emit_tok(p->emit, BIN_OP_ASSIGN);
                advance(p);
                parse_expr(p);
            }
            expect(p, TOK_SEMI);
            emit_tok(p->emit, BIN_SM_SEMI);

        } else if (next == TOK_SEMI || is_stmt_start(next)) {
            /* bare identifier statement: Q5H9; */
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            soft_semi(p);

        } else if (is_binop(next)) {
            /* expression-as-statement: Q618 == "string"; */
            if (p->symtab) symtab_check(p->symtab, name, line, 0);
            emit_id(p->emit, p->sdb, name);
            while (is_binop(p->lex->cur.kind)) {
                uint16_t op = binop_to_bin(p->lex->cur.kind);
                advance(p);
                emit_tok(p->emit, op);
                parse_expr_unary(p);
            }
            soft_semi(p);

        } else {
            parse_err(p, "unexpected token after identifier in statement");
        }

    } else if (k == TOK_NULL_LIT) {
        /* NULL() as a statement — unlikely but handle */
        emit_id(p->emit, p->sdb, "NULL");
        emit_tok(p->emit, BIN_SM_LPAREN);
        emit_tok(p->emit, BIN_SM_RPAREN);
        advance(p);
        expect(p, TOK_SEMI);
        emit_tok(p->emit, BIN_SM_SEMI);

    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "unexpected token kind %d in statement", (int)k);
        parse_err(p, msg);
    }
}

static void parse_stmt(Parser *p)
{
    TokKind k = p->lex->cur.kind;

    if (is_type_kw(k)) { parse_var_decl(p, 0);   return; }
    if (k == KW_MEMBER) { parse_member_decl(p);   return; }

    switch (k) {
    case KW_IF:       parse_if(p);    return;
    case KW_WHILE:    parse_while(p); return;
    case KW_FOR:      parse_for(p);   return;
    case KW_SWITCH:   parse_switch(p); return;
    case KW_RETURN:   parse_return(p); return;
    case KW_BREAK:
        advance(p);
        emit_tok(p->emit, BIN_TK_BREAK);
        expect(p, TOK_SEMI);
        emit_tok(p->emit, BIN_SM_SEMI);
        return;
    case KW_CONTINUE:
        advance(p);
        emit_tok(p->emit, BIN_TK_CONTINUE);
        expect(p, TOK_SEMI);
        emit_tok(p->emit, BIN_SM_SEMI);
        return;
    case KW_GOTO:
        advance(p);
        emit_tok(p->emit, BIN_TK_GOTO);
        if (p->lex->cur.kind != TOK_IDENT)
            parse_err(p, "expected label after goto");
        /* goto label is not a symbol reference — do not check */
        emit_id(p->emit, p->sdb, p->lex->cur.text);
        advance(p);
        expect(p, TOK_SEMI);
        emit_tok(p->emit, BIN_SM_SEMI);
        return;
    case TOK_SEMI:
        advance(p);
        emit_tok(p->emit, BIN_SM_SEMI); /* bare ';' statement */
        return;
    default:
        parse_expr_stmt(p);
    }
}

/* ── Parameter list ────────────────────────────────────────────────────────── */

/*
 * parse_param_list — parse a function parameter list.
 *
 * allow_anon:      allow parameters without names (forward declarations)
 * register_params: if non-zero, register named params as locals in symtab
 */
static void parse_param_list(Parser *p, int allow_anon, int register_params)
{
    if (p->lex->cur.kind == TOK_RPAREN) return;

    for (;;) {
        if (!is_type_kw(p->lex->cur.kind))
            parse_err(p, "expected type in parameter list");
        parse_type(p);
        if (p->lex->cur.kind == TOK_IDENT) {
            if (register_params && p->symtab)
                symtab_add_local(p->symtab, p->lex->cur.text);
            emit_id(p->emit, p->sdb, p->lex->cur.text);
            advance(p);
        } else if (!allow_anon) {
            parse_err(p, "expected parameter name");
        }
        if (p->lex->cur.kind != TOK_COMMA) break;
        advance(p);
        emit_tok(p->emit, BIN_SM_COMMA);
    }
}

/* ── Top-level declarations ────────────────────────────────────────────────── */

static void parse_inherits(Parser *p)
{
    /* 'inherits' IDENT ';' */
    expect(p, KW_INHERITS);
    emit_tok(p->emit, BIN_TK_INHERITS);
    if (p->lex->cur.kind != TOK_IDENT)
        parse_err(p, "expected name after 'inherits'");
    emit_id(p->emit, p->sdb, p->lex->cur.text);
    advance(p);
    expect(p, TOK_SEMI);
    emit_tok(p->emit, BIN_SM_SEMI);
}

static void parse_forward(Parser *p)
{
    /* 'forward' type IDENT '(' forwardParamList ')' ';' */
    expect(p, KW_FORWARD);
    emit_tok(p->emit, BIN_TK_FORWARD);
    parse_type(p);
    if (p->lex->cur.kind != TOK_IDENT)
        parse_err(p, "expected name after forward type");
    emit_id(p->emit, p->sdb, p->lex->cur.text);
    advance(p);
    expect(p, TOK_LPAREN);
    emit_tok(p->emit, BIN_SM_LPAREN);
    parse_param_list(p, 1 /* allow_anon */, 0 /* no registration */);
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);
    expect(p, TOK_SEMI);
    emit_tok(p->emit, BIN_SM_SEMI);
}

static void parse_function(Parser *p)
{
    /*
     * 'function' [type] IDENT '(' paramList ')' block
     * 'function' [type] IDENT '(' paramList ')' ';'  — forward-declare form
     */
    expect(p, KW_FUNCTION);
    emit_tok(p->emit, BIN_TK_FUNCTION);
    /* optional return type */
    if (is_type_kw(p->lex->cur.kind))
        parse_type(p);
    if (p->lex->cur.kind != TOK_IDENT)
        parse_err(p, "expected function name");
    emit_id(p->emit, p->sdb, p->lex->cur.text);
    advance(p);
    expect(p, TOK_LPAREN);
    emit_tok(p->emit, BIN_SM_LPAREN);

    /* Push a scope so params and locals are tracked inside the body.
     * For the forward-declare form (';' at end) we pop immediately. */
    if (p->symtab) symtab_push_scope(p->symtab);
    parse_param_list(p, 0 /* named params required */, 1 /* register */);
    expect(p, TOK_RPAREN);
    emit_tok(p->emit, BIN_SM_RPAREN);

    if (p->lex->cur.kind == TOK_SEMI) {
        /* forward-declare form */
        advance(p);
        emit_tok(p->emit, BIN_SM_SEMI);
        if (p->symtab) symtab_pop_scope(p->symtab);
    } else {
        parse_block(p);
        if (p->symtab) symtab_pop_scope(p->symtab);
    }
}

static void parse_trigger(Parser *p)
{
    /*
     * 'trigger' IDENT block
     * 'trigger' IDENT '(' filterExpr ')' block
     * 'trigger' INT IDENT block   (integer channel prefix)
     */
    expect(p, KW_TRIGGER);
    emit_tok(p->emit, BIN_TK_TRIGGER);

    /* optional integer channel prefix: trigger 0x64 name */
    if (p->lex->cur.kind == TOK_INT) {
        int hd = p->lex->cur.ihexdigits;
        emit_int(p->emit, p->lex->cur.ival, hd);
        advance(p);
    }

    if (p->lex->cur.kind != TOK_IDENT)
        parse_err(p, "expected trigger name");
    emit_id(p->emit, p->sdb, p->lex->cur.text);
    advance(p);

    /* Push scope for the trigger body (and filter expression if any) */
    if (p->symtab) symtab_push_scope(p->symtab);

    if (p->lex->cur.kind == TOK_LPAREN) {
        advance(p);
        emit_tok(p->emit, BIN_SM_LPAREN);
        parse_expr(p);
        expect(p, TOK_RPAREN);
        emit_tok(p->emit, BIN_SM_RPAREN);
    }

    parse_block(p);
    if (p->symtab) symtab_pop_scope(p->symtab);
}

/* ── Script ────────────────────────────────────────────────────────────────── */

static void parse_script(Parser *p)
{
    while (p->lex->cur.kind != TOK_EOF) {
        TokKind k = p->lex->cur.kind;
        if      (k == KW_INHERITS) parse_inherits(p);
        else if (k == KW_MEMBER)   parse_member_decl(p);
        else if (k == KW_FORWARD)  parse_forward(p);
        else if (k == KW_FUNCTION) parse_function(p);
        else if (k == KW_TRIGGER)  parse_trigger(p);
        else {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "unexpected token kind %d at top level", (int)k);
            parse_err(p, msg);
        }
    }
}

/* ── Public entry point ────────────────────────────────────────────────────── */

void compile(Lexer *lex, Emit *emit, SDB *sdb, SymTab *symtab, EnumDB *enumdb)
{
    Parser p;
    p.lex        = lex;
    p.emit       = emit;
    p.sdb        = sdb;
    p.symtab     = symtab;
    p.enumdb     = enumdb;
    p.call_depth = 0;
    parse_script(&p);
}
