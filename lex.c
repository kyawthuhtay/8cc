// Copyright 2012 Rui Ueyama <rui314@gmail.com>
// This program is free software licensed under the MIT license.

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "8cc.h"

static bool at_bol = true;

typedef struct {
    char *displayname;
    char *realname;
    int line;
    int column;
    FILE *fp;
} File;

static Vector *buffer = &EMPTY_VECTOR;
static Vector *altbuffer = NULL;
static Vector *file_stack = &EMPTY_VECTOR;
static File *file;
static int line_mark = -1;
static int column_mark = -1;
static int ungotten = -1;

static Token *newline_token = &(Token){ .kind = TNEWLINE, .space = false };

static void skip_block_comment(void);

static File *make_file(char *displayname, char *realname, FILE *fp) {
    File *r = malloc(sizeof(File));
    r->displayname = displayname;
    r->realname = realname;
    r->line = 1;
    r->column = 0;
    r->fp = fp;
    return r;
}

void lex_init(char *filename) {
    if (!strcmp(filename, "-")) {
        set_input_file("(stdin)", NULL, stdin);
        return;
    }
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        char buf[128];
        strerror_r(errno, buf, sizeof(buf));
        error("Cannot open %s: %s", filename, buf);
    }
    set_input_file(filename, filename, fp);
}

static Token *make_token(Token *tmpl) {
    Token *r = malloc(sizeof(Token));
    *r = *tmpl;
    r->hideset = make_map();
    r->file = file->displayname;
    r->line = (line_mark < 0) ? file->line : line_mark;
    r->column = (column_mark < 0) ? file->column : column_mark;
    line_mark = -1;
    column_mark = -1;
    return r;
}

static Token *make_ident(char *p) {
    return make_token(&(Token){ TIDENT, .space = false, .sval = p });
}

static Token *make_strtok(char *s) {
    return make_token(&(Token){ TSTRING, .space = false, .sval = s });
}

static Token *make_keyword(int id) {
    return make_token(&(Token){ TKEYWORD, .space = false, .id = id });
}

static Token *make_number(char *s) {
    return make_token(&(Token){ TNUMBER, .space = false, .sval = s });
}

static Token *make_char(char c) {
    return make_token(&(Token){ TCHAR, .space = false, .c = c });
}

static Token *make_space() {
    return make_token(&(Token){ TSPACE });
}

void push_input_file(char *displayname, char *realname, FILE *fp) {
    vec_push(file_stack, file);
    file = make_file(displayname, realname, fp);
    at_bol = true;
}

void set_input_file(char *displayname, char *realname, FILE *fp) {
    file = make_file(displayname, realname, fp);
    at_bol = true;
}

char *input_position(void) {
    if (!file)
        return "(unknown)";
    return format("%s:%d:%d", file->displayname, file->line, file->column);
}

char *get_current_file(void) {
    return file->realname;
}

int get_current_line(void) {
    return file->line;
}

void set_current_line(int line) {
    file->line = line;
}

char *get_current_displayname(void) {
    return file->displayname;
}

void set_current_displayname(char *name) {
    file->displayname = name;
}

static void mark_input(void) {
    line_mark = file->line;
    column_mark = file->column;
}

static void unget(int c) {
    if (c == '\n')
        file->line--;
    if (ungotten >= 0)
        ungetc(ungotten, file->fp);
    ungotten = c;
    file->column--;
}

static bool iswhitespace(int c) {
    return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

static bool skip_whitespace(void) {
    bool r = false;
    for (;;) {
        int c = getc(file->fp);
        if (iswhitespace(c)) {
            file->column++;
            r = true;
            continue;
        }
        ungetc(c, file->fp);
        return r;
    }
}

static bool skip_newline(int c) {
    if (c == '\n')
        return true;
    if (c == '\r') {
        c = getc(file->fp);
        if (c == '\n')
            return true;
        ungetc(c, file->fp);
        return true;
    }
    return false;
}

static int get(void) {
    int c = (ungotten >= 0) ? ungotten : getc(file->fp);
    file->column++;
    ungotten = -1;
    if (c == '\\') {
        bool space_exist = skip_whitespace();
        c = getc(file->fp);
        if (skip_newline(c)) {
            if (space_exist)
                warn("backslash and newline separated by space");
            file->line++;
            file->column = 0;
            return get();
        }
        unget(c);
        at_bol = false;
        return '\\';
    }
    if (skip_newline(c)) {
        file->line++;
        file->column = 0;
        at_bol = true;
    } else {
        at_bol = false;
    }
    return c;
}

static int peek(void) {
    int r = get();
    unget(r);
    return r;
}

static bool next(int expect) {
    int c = get();
    if (c == expect)
        return true;
    unget(c);
    return false;
}

static void skip_line(void) {
    for (;;) {
        int c = get();
        if (c == EOF)
            return;
        if (c == '\n' || c == '\r') {
            unget(c);
            return;
        }
    }
}

static bool skip_space(void) {
    bool r = false;
    for (;;) {
        int c = get();
        if (c == EOF)
            break;
        if (iswhitespace(c)) {
            r = true;
            continue;
        }
        if (c == '/') {
            if (next('*')) {
                skip_block_comment();
                r = true;
                continue;
            } else if (next('/')) {
                skip_line();
                r = true;
                continue;
            }
            unget('/');
            break;
        }
        unget(c);
        break;
    }
    return r;
}

void skip_cond_incl(void) {
    int nest = 0;
    for (;;) {
        skip_space();
        int c = get();
        if (c == EOF)
            return;
        if (c == '\n' || c == '\r')
            continue;
        if (c != '#') {
            skip_line();
            continue;
        }
        skip_space();
        Token *tok = lex();
        if (tok->kind == TNEWLINE)
            continue;
        if (tok->kind != TIDENT) {
            skip_line();
            continue;
        }
        if (!nest && (is_ident(tok, "else") || is_ident(tok, "elif") || is_ident(tok, "endif"))) {
            unget_token(tok);
            Token *sharp = make_keyword('#');
            sharp->bol = true;
            unget_token(sharp);
            return;
        }
        if (is_ident(tok, "if") || is_ident(tok, "ifdef") || is_ident(tok, "ifndef"))
            nest++;
        else if (nest && is_ident(tok, "endif"))
            nest--;
        skip_line();
    }
}

static Token *read_number(char c) {
    Buffer *b = make_buffer();
    buf_write(b, c);
    char last = c;
    for (;;) {
        int c = get();
        bool flonum = strchr("eEpP", last) && strchr("+-", c);
        if (!isdigit(c) && !isalpha(c) && c != '.' && !flonum) {
            unget(c);
            buf_write(b, '\0');
            return make_number(buf_body(b));
        }
        buf_write(b, c);
        last = c;
    }
}

static int read_octal_char(int c) {
    int r = c - '0';
    c = get();
    if ('0' <= c && c <= '7') {
        r = (r << 3) | (c - '0');
        c = get();
        if ('0' <= c && c <= '7')
            r = (r << 3) | (c - '0');
        else
            unget(c);
    } else {
        unget(c);
    }
    return r;
}

static int read_hex_char(void) {
    int c = get();
    int r = 0;
    if (!isxdigit(c))
        error("\\x is not followed by a hexadecimal character: %c", c);
    for (;; c = get()) {
        switch (c) {
        case '0' ... '9': r = (r << 4) | (c - '0'); continue;
        case 'a' ... 'f': r = (r << 4) | (c - 'a' + 10); continue;
        case 'A' ... 'F': r = (r << 4) | (c - 'A' + 10); continue;
        default: unget(c); return r;
        }
    }
}

static bool is_valid_ucn(unsigned int c) {
    if (0xD800 <= c && c <= 0xDFFF)
        return false;
    return 0xA0 <= c || c == '$' || c == '@' || c == '`';
}

static int read_universal_char(int len) {
    unsigned int r = 0;
    for (int i = 0; i < len; i++) {
        char c = get();
        switch (c) {
        case '0' ... '9': r = (r << 4) | (c - '0'); continue;
        case 'a' ... 'f': r = (r << 4) | (c - 'a' + 10); continue;
        case 'A' ... 'F': r = (r << 4) | (c - 'A' + 10); continue;
        default: error("invalid universal character: %c", c);
        }
    }
    if (!is_valid_ucn(r))
        error("invalid universal character: \\%c%0*x", (len == 4) ? 'u' : 'U', len, r);
    return r;
}

static int read_escaped_char(void) {
    int c = get();
    switch (c) {
    case '\'': case '"': case '?': case '\\':
        return c;
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case 'e': return '\033'; // '\e' is GNU extension
    case '0' ... '7':
        return read_octal_char(c);
    case 'x':
        return read_hex_char();
    case 'u':
        return read_universal_char(4);
    case 'U':
        return read_universal_char(8);
    case EOF:
        error("premature end of input");
    default:
        warn("unknown escape character: \\%c", c);
        return c;
    }
}

static Token *read_char(void) {
    int c = get();
    char r = (c == '\\') ? read_escaped_char() : c;
    c = get();
    if (c == EOF)
        error("premature end of input");
    if (c != '\'')
        error("unterminated string: %c", c);
    return make_char(r);
}

static Token *read_string(void) {
    Buffer *b = make_buffer();
    for (;;) {
        int c = get();
        if (c == EOF)
            error("Unterminated string");
        if (c == '"')
            break;
        if (c == '\\')
            c = read_escaped_char();
        buf_write(b, c);
    }
    buf_write(b, '\0');
    return make_strtok(buf_body(b));
}

static Token *read_ident(char c) {
    Buffer *b = make_buffer();
    buf_write(b, c);
    for (;;) {
        c = get();
        if (isalnum(c) || c == '_' || c == '$') {
            buf_write(b, c);
            continue;
        }
        unget(c);
        buf_write(b, '\0');
        return make_ident(buf_body(b));
    }
}

static void skip_block_comment(void) {
    enum { in_comment, asterisk_read } state = in_comment;
    for (;;) {
        int c = get();
        if (c == EOF)
            error("premature end of block comment");
        if (c == '*')
            state = asterisk_read;
        else if (state == asterisk_read && c == '/')
            return;
        else
            state = in_comment;
    }
}

static Token *read_rep(char expect, int t1, int els) {
    return make_keyword(next(expect) ? t1 : els);
}

static Token *read_rep2(char expect1, int t1, char expect2, int t2, char els) {
    if (next(expect1))
        return make_keyword(t1);
    return make_keyword(next(expect2) ? t2 : els);
}

static Token *do_read_token(void) {
    mark_input();
    int c = get();
    switch (c) {
    case ' ': case '\t': case '\v': case '\f':
        skip_space();
        return make_space();
    case '\n': case '\r':
        skip_newline(c);
        return newline_token;
    case 'L':
        if (next('"'))  return read_string();
        if (next('\'')) return read_char();
        // fallthrough
    case 'a' ... 'z': case 'A' ... 'K': case 'M' ... 'Z': case '_': case '$':
        return read_ident(c);
    case '0' ... '9':
        return read_number(c);
    case '/':
        if (next('/')) {
            skip_line();
            return make_space();
        }
        if (next('*')) {
            skip_block_comment();
            return make_space();
        }
        return make_keyword(next('=') ? OP_A_DIV : '/');
    case '.':
        if (isdigit(peek()))
            return read_number(c);
        if (next('.')) {
            if (next('.'))
                return make_keyword(KTHREEDOTS);
            return make_ident("..");
        }
        return make_keyword('.');
    case '(': case ')': case ',': case ';': case '[': case ']': case '{':
    case '}': case '?': case '~':
        return make_keyword(c);
    case ':':
        return make_keyword(next('>') ? ']' : ':');
    case '#':
        return make_keyword(next('#') ? KSHARPSHARP : '#');
    case '+':
        return read_rep2('+', OP_INC, '=', OP_A_ADD, '+');
    case '-':
        if (next('-')) return make_keyword(OP_DEC);
        if (next('>')) return make_keyword(OP_ARROW);
        if (next('=')) return make_keyword(OP_A_SUB);
        return make_keyword('-');
    case '*':
        return read_rep('=', OP_A_MUL, '*');
    case '%':
        if (next('>'))
            return make_keyword('}');
        if (next(':')) {
            if (next('%')) {
                if (next(':'))
                    return make_keyword(KSHARPSHARP);
                unget('%');
            }
            return make_keyword('#');
        }
        return read_rep('=', OP_A_MOD, '%');
    case '=': return read_rep('=', OP_EQ, '=');
    case '!': return read_rep('=', OP_NE, '!');
    case '&': return read_rep2('&', OP_LOGAND, '=', OP_A_AND, '&');
    case '|': return read_rep2('|', OP_LOGOR, '=', OP_A_OR, '|');
    case '^': return read_rep('=', OP_A_XOR, '^');
    case '<':
        if (next('<')) return read_rep('=', OP_A_SAL, OP_SAL);
        if (next('=')) return make_keyword(OP_LE);
        if (next(':')) return make_keyword('[');
        if (next('%')) return make_keyword('{');
        return make_keyword('<');
    case '>':
        if (next('=')) return make_keyword(OP_GE);
        if (next('>')) return read_rep('=', OP_A_SAR, OP_SAR);
        return make_keyword('>');
    case '"': return read_string();
    case '\'': return read_char();
    case EOF:
        return NULL;
    default:
        error("Unexpected character: '%c'", c);
    }
}

char *read_header_file_name(bool *std) {
    skip_space();
    char close;
    if (next('"')) {
        *std = false;
        close = '"';
    } else if (next('<')) {
        *std = true;
        close = '>';
    } else {
        return NULL;
    }
    Buffer *b = make_buffer();
    for (;;) {
        int c = get();
        if (c == EOF || c == '\n' || c == '\r')
            error("premature end of header name");
        if (c == close)
            break;
        buf_write(b, c);
    }
    if (buf_len(b) == 0)
        error("header name should not be empty");
    buf_write(b, '\0');
    return buf_body(b);
}

bool is_keyword(Token *tok, int c) {
    return tok && (tok->kind == TKEYWORD) && (tok->id == c);
}

void set_input_buffer(Vector *tokens) {
    altbuffer = tokens;
}

Vector *get_input_buffer(void) {
    return altbuffer;
}

char *read_error_directive(void) {
    Buffer *b = make_buffer();
    bool bol = true;
    for (;;) {
        int c = get();
        if (c == EOF)
            break;
        if (c == '\n' || c == '\r') {
            unget(c);
            break;
        }
        if (bol && iswhitespace(c))
            continue;
        bol = false;
        buf_write(b, c);
    }
    buf_write(b, '\0');
    return buf_body(b);
}

void unget_token(Token *tok) {
    if (!tok) return;
    vec_push(altbuffer ? altbuffer : buffer, tok);
}

Token *lex(void) {
    if (altbuffer) {
        if (vec_len(altbuffer) == 0)
            return NULL;
        return vec_pop(altbuffer);
    }
    if (vec_len(buffer) > 0)
        return vec_pop(buffer);
    bool bol = at_bol;
    Token *tok = do_read_token();
    while (tok && tok->kind == TSPACE) {
        tok = do_read_token();
        if (tok)
            tok->space = true;
    }
    if (!tok && vec_len(file_stack) > 0) {
        fclose(file->fp);
        file = vec_pop(file_stack);
        at_bol = true;
        return newline_token;
    }
    if (tok) tok->bol = bol;
    return tok;
}
