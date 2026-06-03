#ifndef TOKENS_H_
#define TOKENS_H_

#include <stdint.h>

/* ── Source lexer token kinds ─────────────────────────────────────────────── */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_INT,
    TOK_STRING,
    TOK_USTRING,    /* L"..." */
    TOK_NULL_LIT,   /* NULL() — matched as one token */

    /* Type keywords */
    KW_INT, KW_STRING, KW_USTRING, KW_LOC, KW_OBJ, KW_LIST, KW_VOID,
    KW_FLOAT, KW_STR,   /* mapped to nearest binary type */

    /* Control / declaration keywords */
    KW_IF, KW_ELSE, KW_WHILE, KW_FOR,
    KW_BREAK, KW_CONTINUE, KW_GOTO,
    KW_SWITCH, KW_CASE, KW_DEFAULT,
    KW_RETURN,
    KW_FUNCTION, KW_TRIGGER, KW_MEMBER, KW_INHERITS, KW_FORWARD,

    /* Operators */
    TOK_ASSIGN,     /* =   */
    TOK_EQ,         /* ==  */
    TOK_NEQ,        /* !=  */
    TOK_LT,         /* <   */
    TOK_GT,         /* >   */
    TOK_LTEQ,       /* <=  */
    TOK_GTEQ,       /* >=  */
    TOK_ADD,        /* +   */
    TOK_SUB,        /* -   */
    TOK_MUL,        /* *   */
    TOK_DIV,        /* /   */
    TOK_MOD,        /* %   */
    TOK_NOT,        /* !   */
    TOK_AND,        /* &&  */
    TOK_OR,         /* ||  */
    TOK_XOR,        /* ^   */
    TOK_INC,        /* ++  */
    TOK_DEC,        /* --  */
    TOK_ADD_ASSIGN, /* +=  */
    TOK_SUB_ASSIGN, /* -=  */
    TOK_MUL_ASSIGN, /* *=  */
    TOK_DIV_ASSIGN, /* /=  */

    /* Punctuation */
    TOK_LPAREN,     /* (  */
    TOK_RPAREN,     /* )  */
    TOK_LBRACE,     /* {  */
    TOK_RBRACE,     /* }  */
    TOK_LBRACKET,   /* [  */
    TOK_RBRACKET,   /* ]  */
    TOK_SEMI,       /* ;  */
    TOK_COMMA,      /* ,  */
    TOK_COLON,      /* :  */
    TOK_DOT,        /* .  */
} TokKind;

/* ── Binary token variant-0 values (little-endian uint16_t) ──────────────── */

#define BIN_SM_LPAREN    0x390Cu
#define BIN_SM_RPAREN    0x39B3u
#define BIN_SM_COMMA     0x3B25u
#define BIN_SM_SEMI      0x0732u
#define BIN_SM_LBRACE    0x3A9Eu
#define BIN_SM_RBRACE    0x7EB7u
#define BIN_SM_LBRACKET  0x409Du
#define BIN_SM_RBRACKET  0x0902u

#define BIN_OP_NOT       0x3CD5u
#define BIN_OP_ADD       0x23C9u
#define BIN_OP_SUB       0x047Eu
#define BIN_OP_MUL       0x261Eu
#define BIN_OP_DIV       0x0384u
#define BIN_OP_MOD       0x249Eu
#define BIN_OP_ISEQ      0x0135u
#define BIN_OP_ISNEQ     0x0E90u
#define BIN_OP_LT        0x37E5u
#define BIN_OP_GT        0x16D4u
#define BIN_OP_LTEQ      0x3807u
#define BIN_OP_GTEQ      0x19DAu
#define BIN_OP_ASSIGN    0x1F16u
#define BIN_OP_LOGAND    0x5D24u
#define BIN_OP_LOGOR     0x13D3u
#define BIN_OP_XOR       0x263Du
#define BIN_OP_INC       0x5CCDu
#define BIN_OP_DEC       0x2528u

#define BIN_TK_INT       0x30F1u
#define BIN_TK_STRING    0x3F9Au
#define BIN_TK_USTRING   0x10D9u
#define BIN_TK_LOC       0x01E1u
#define BIN_TK_OBJ       0x28E2u
#define BIN_TK_LIST      0x26B1u
#define BIN_TK_VOID      0x2E39u

#define BIN_TK_IF        0x12C2u
#define BIN_TK_ELSE      0x3305u
#define BIN_TK_WHILE     0x3106u
#define BIN_TK_FOR       0x7DAAu
#define BIN_TK_BREAK     0x7F0Du
#define BIN_TK_CONTINUE  0x017Bu
#define BIN_TK_GOTO      0x190Bu
#define BIN_TK_SWITCH    0x04B0u
#define BIN_TK_CASE      0x3223u
#define BIN_TK_DEFAULT   0x5D3Du
#define BIN_TK_RETURN    0x5DE9u
#define BIN_TK_FUNCTION  0x0E01u
#define BIN_TK_TRIGGER   0x09B3u
#define BIN_TK_MEMBER    0x0C95u
#define BIN_TK_INHERITS  0x31BEu
#define BIN_TK_FORWARD   0x2934u

#define BIN_T_STR        0x5D17u
#define BIN_T_BYTE       0x17BDu
#define BIN_T_WORD       0x5B60u
#define BIN_T_DWORD      0x188Fu
#define BIN_T_ID         0x3510u

#endif /* TOKENS_H_ */
