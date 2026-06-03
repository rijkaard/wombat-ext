#ifndef LEX_H_
#define LEX_H_

#include "tokens.h"
#include <stdint.h>

#define MAX_TOK_TEXT 8192

typedef struct {
    TokKind kind;
    char    text[MAX_TOK_TEXT]; /* identifier / string value (no quotes) */
    int64_t ival;               /* integer value */
    int     ihexdigits;         /* hex digit count for 0x literals (0 = decimal) */
    int     line;
} Token;

typedef struct {
    const char *src;
    const char *pos;
    int         line;
    Token       cur;
} Lexer;

void lex_init(Lexer *l, const char *src);
void lex_advance(Lexer *l);

#endif /* LEX_H_ */
