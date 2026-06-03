#include "lex.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *word; TokKind kind; } KwEntry;
static const KwEntry KEYWORDS[] = {
    {"int",      KW_INT},
    {"string",   KW_STRING},
    {"ustring",  KW_USTRING},
    {"loc",      KW_LOC},
    {"obj",      KW_OBJ},
    {"list",     KW_LIST},
    {"void",     KW_VOID},
    {"float",    KW_FLOAT},
    {"str",      KW_STR},
    {"if",       KW_IF},
    {"else",     KW_ELSE},
    {"while",    KW_WHILE},
    {"for",      KW_FOR},
    {"break",    KW_BREAK},
    {"continue", KW_CONTINUE},
    {"goto",     KW_GOTO},
    {"switch",   KW_SWITCH},
    {"case",     KW_CASE},
    {"default",  KW_DEFAULT},
    {"return",   KW_RETURN},
    {"function", KW_FUNCTION},
    {"trigger",  KW_TRIGGER},
    {"member",   KW_MEMBER},
    {"inherits", KW_INHERITS},
    {"forward",  KW_FORWARD},
};
#define NUM_KW (int)(sizeof(KEYWORDS)/sizeof(KEYWORDS[0]))

static TokKind lookup_kw(const char *s)
{
    for (int i = 0; i < NUM_KW; i++)
        if (strcmp(s, KEYWORDS[i].word) == 0)
            return KEYWORDS[i].kind;
    return TOK_IDENT;
}

void lex_init(Lexer *l, const char *src)
{
    l->src  = src;
    l->pos  = src;
    l->line = 1;
    memset(&l->cur, 0, sizeof(l->cur));
    l->cur.line = 1;
    lex_advance(l);
}

void lex_advance(Lexer *l)
{
    const char *p = l->pos;
    Token *t = &l->cur;

restart:
    /* skip whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        if (*p == '\n') l->line++;
        p++;
    }

    /* skip comments */
    if (p[0] == '/' && p[1] == '/') {
        while (*p && *p != '\n') p++;
        goto restart;
    }
    if (p[0] == '/' && p[1] == '*') {
        p += 2;
        while (*p && !(p[0] == '*' && p[1] == '/')) {
            if (*p == '\n') l->line++;
            p++;
        }
        if (*p) p += 2;
        goto restart;
    }

    t->line = l->line;

    if (!*p) {
        t->kind = TOK_EOF;
        l->pos  = p;
        return;
    }

    /* NULL() — special combined token */
    if (strncmp(p, "NULL()", 6) == 0) {
        t->kind = TOK_NULL_LIT;
        t->text[0] = '\0';
        l->pos = p + 6;
        return;
    }

    /* L"..." wide string */
    if (p[0] == 'L' && p[1] == '"') {
        p += 2;
        size_t n = 0;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { p++; }
            if (n < MAX_TOK_TEXT - 1) t->text[n++] = *p;
            if (*p == '\n') l->line++;
            p++;
        }
        if (*p == '"') p++;
        t->text[n] = '\0';
        t->kind = TOK_USTRING;
        l->pos  = p;
        return;
    }

    /* String literal */
    if (*p == '"') {
        p++;
        size_t n = 0;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                char c;
                switch (*p) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                default:   c = *p;  break;
                }
                if (n < MAX_TOK_TEXT - 1) t->text[n++] = c;
            } else {
                if (*p == '\n') l->line++;
                if (n < MAX_TOK_TEXT - 1) t->text[n++] = *p;
            }
            p++;
        }
        if (*p == '"') p++;
        t->text[n] = '\0';
        t->kind = TOK_STRING;
        l->pos  = p;
        return;
    }

    /* Integer literal */
    if (isdigit((unsigned char)*p)) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            t->ival = 0;
            t->ihexdigits = 0;
            while (isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : toupper(*p) - 'A' + 10;
                t->ival = t->ival * 16 + d;
                t->ihexdigits++;
                p++;
            }
        } else {
            t->ival = 0;
            t->ihexdigits = 0; /* decimal */
            while (isdigit((unsigned char)*p))
                t->ival = t->ival * 10 + (*p++ - '0');
        }
        t->kind = TOK_INT;
        l->pos  = p;
        return;
    }

    /* Negative integer: handled by unary minus in parser */

    /* Identifier or keyword */
    if (isalpha((unsigned char)*p) || *p == '_') {
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '_') {
            if (n < MAX_TOK_TEXT - 1) t->text[n++] = *p;
            p++;
        }
        t->text[n] = '\0';
        t->kind = lookup_kw(t->text);
        l->pos  = p;
        return;
    }

    /* Operators and punctuation */
    switch (*p) {
    case '(':  t->kind = TOK_LPAREN;   p++; break;
    case ')':  t->kind = TOK_RPAREN;   p++; break;
    case '{':  t->kind = TOK_LBRACE;   p++; break;
    case '}':  t->kind = TOK_RBRACE;   p++; break;
    case '[':  t->kind = TOK_LBRACKET; p++; break;
    case ']':  t->kind = TOK_RBRACKET; p++; break;
    case ';':  t->kind = TOK_SEMI;     p++; break;
    case ',':  t->kind = TOK_COMMA;    p++; break;
    case ':':  t->kind = TOK_COLON;    p++; break;
    case '.':  t->kind = TOK_DOT;      p++; break;
    case '^':  t->kind = TOK_XOR;      p++; break;
    case '~':  t->kind = TOK_NOT;      p++; break; /* treat ~ as ! */
    case '!':
        if (p[1] == '=') { t->kind = TOK_NEQ;   p += 2; }
        else              { t->kind = TOK_NOT;   p++;    }
        break;
    case '=':
        if (p[1] == '=') { t->kind = TOK_EQ;    p += 2; }
        else              { t->kind = TOK_ASSIGN; p++;   }
        break;
    case '<':
        if (p[1] == '=') { t->kind = TOK_LTEQ;  p += 2; }
        else              { t->kind = TOK_LT;    p++;    }
        break;
    case '>':
        if (p[1] == '=') { t->kind = TOK_GTEQ;  p += 2; }
        else              { t->kind = TOK_GT;    p++;    }
        break;
    case '&':
        if (p[1] == '&') { t->kind = TOK_AND;   p += 2; }
        else              { p++; goto restart; }          /* skip lone & */
        break;
    case '|':
        if (p[1] == '|') { t->kind = TOK_OR;    p += 2; }
        else              { p++; goto restart; }          /* skip lone | */
        break;
    case '+':
        if (p[1] == '+') { t->kind = TOK_INC;        p += 2; }
        else if (p[1]=='=') { t->kind = TOK_ADD_ASSIGN; p += 2; }
        else              { t->kind = TOK_ADD;        p++;    }
        break;
    case '-':
        if (p[1] == '-') { t->kind = TOK_DEC;        p += 2; }
        else if (p[1]=='=') { t->kind = TOK_SUB_ASSIGN; p += 2; }
        else              { t->kind = TOK_SUB;        p++;    }
        break;
    case '*':
        if (p[1] == '=') { t->kind = TOK_MUL_ASSIGN; p += 2; }
        else              { t->kind = TOK_MUL;        p++;    }
        break;
    case '/':
        if (p[1] == '=') { t->kind = TOK_DIV_ASSIGN; p += 2; }
        else              { t->kind = TOK_DIV;        p++;    }
        break;
    case '%':
        t->kind = TOK_MOD; p++;
        break;
    default:
        fprintf(stderr, "lex: unexpected character '%c' (0x%02x) at line %d\n",
                *p, (unsigned char)*p, l->line);
        p++;
        goto restart;
    }

    l->pos = p;
}
