#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The lexer keeps source spans for ordinary tokens and decodes only quoted
 * strings/identifiers. No parser result borrows memory from the input SQL. */
typedef enum {
    TK_END, TK_NAME, TK_STRING, TK_NUMBER, TK_LPAREN, TK_RPAREN, TK_COMMA,
    TK_SEMICOLON, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE
} TokenKind;

typedef struct {
    TokenKind kind;
    size_t start, length;
    char *decoded;
    bool quoted;
} Token;

typedef struct {
    const char *sql;
    size_t length, position;
    Token token;
    ADBError *error;
    bool failed;
    size_t depth, nodes;
} Parser;

static bool fail(Parser *parser, ADBCode code, const char *message)
{
    if (!parser->failed) {
        adb_error(parser->error, code, "%s", message);
        if (parser->error) parser->error->offset = parser->token.start;
        parser->failed = true;
    }
    return false;
}

static bool keyword(const Parser *parser, const char *word)
{
    const Token *token = &parser->token;
    if (token->kind != TK_NAME || token->quoted || strlen(word) != token->length)
        return false;
    for (size_t i = 0; i < token->length; ++i)
        if (toupper((unsigned char)parser->sql[token->start + i]) !=
            toupper((unsigned char)word[i])) return false;
    return true;
}

static bool identifier_start(unsigned char character)
{
    return isalpha(character) || character == '_';
}

static bool identifier_part(unsigned char character)
{
    return isalnum(character) || character == '_' || character == '$';
}

static void next(Parser *parser)
{
    free(parser->token.decoded);
    memset(&parser->token, 0, sizeof parser->token);
    if (parser->failed) return;
    const char *sql = parser->sql;
    for (;;) {
        while (parser->position < parser->length &&
               isspace((unsigned char)sql[parser->position])) ++parser->position;
        size_t position = parser->position;
        if (position + 1 < parser->length && sql[position] == '-' && sql[position + 1] == '-') {
            parser->position += 2;
            while (parser->position < parser->length && sql[parser->position] != '\n') ++parser->position;
        } else if (position + 1 < parser->length && sql[position] == '/' && sql[position + 1] == '*') {
            parser->position += 2;
            while (parser->position + 1 < parser->length &&
                   !(sql[parser->position] == '*' && sql[parser->position + 1] == '/')) ++parser->position;
            if (parser->position + 1 >= parser->length) {
                parser->token.start = position;
                fail(parser, ADB_ERR_PARSE, "Unterminated block comment");
                return;
            }
            parser->position += 2;
        } else break;
    }
    Token *token = &parser->token;
    token->start = parser->position;
    if (parser->position == parser->length) { token->kind = TK_END; return; }
    unsigned char character = (unsigned char)sql[parser->position++];
    if (identifier_start(character)) {
        token->kind = TK_NAME;
        while (parser->position < parser->length &&
               identifier_part((unsigned char)sql[parser->position])) ++parser->position;
        token->length = parser->position - token->start;
        if (token->length > 255) fail(parser, ADB_ERR_LIMIT, "Identifier exceeds 255 bytes");
        return;
    }
    if (character == '\'' || character == '"' || character == '`') {
        char quote = (char)character;
        token->kind = quote == '\'' ? TK_STRING : TK_NAME;
        token->quoted = quote != '\'';
        ADBBuffer decoded = {0};
        bool closed = false;
        while (parser->position < parser->length) {
            char item = sql[parser->position++];
            if (item == quote) {
                if (parser->position < parser->length && sql[parser->position] == quote) ++parser->position;
                else { closed = true; break; }
            }
            if (!adb_buffer_append(&decoded, &item, 1)) {
                adb_buffer_free(&decoded);
                fail(parser, ADB_ERR_MEMORY, "Unable to decode quoted token");
                return;
            }
            if (decoded.length > (token->quoted ? 255u : ADB_MAX_TEXT)) {
                adb_buffer_free(&decoded);
                fail(parser, ADB_ERR_LIMIT, "Quoted token exceeds its size limit");
                return;
            }
        }
        if (!closed) {
            adb_buffer_free(&decoded);
            fail(parser, ADB_ERR_PARSE, "Unterminated quoted token");
            return;
        }
        token->length = decoded.length;
        if (token->quoted && token->length == 0) {
            adb_buffer_free(&decoded);
            fail(parser, ADB_ERR_PARSE, "An identifier cannot be empty");
            return;
        }
        if (!adb_buffer_append(&decoded, "", 1)) {
            adb_buffer_free(&decoded);
            fail(parser, ADB_ERR_MEMORY, "Unable to decode quoted token");
            return;
        }
        token->decoded = decoded.data;
        return;
    }
    if (isdigit(character) || (character == '.' && parser->position < parser->length &&
                              isdigit((unsigned char)sql[parser->position]))) {
        token->kind = TK_NUMBER;
        while (parser->position < parser->length && isdigit((unsigned char)sql[parser->position])) ++parser->position;
        if (parser->position < parser->length && sql[parser->position] == '.' && character != '.') {
            ++parser->position;
            while (parser->position < parser->length && isdigit((unsigned char)sql[parser->position])) ++parser->position;
        }
        if (parser->position < parser->length && (sql[parser->position] == 'e' || sql[parser->position] == 'E')) {
            ++parser->position;
            if (parser->position < parser->length && (sql[parser->position] == '+' || sql[parser->position] == '-')) ++parser->position;
            size_t exponent = parser->position;
            while (parser->position < parser->length && isdigit((unsigned char)sql[parser->position])) ++parser->position;
            if (parser->position == exponent) {
                fail(parser, ADB_ERR_PARSE, "An exponent requires digits");
                return;
            }
        }
        token->length = parser->position - token->start;
        if (token->length > 128) fail(parser, ADB_ERR_LIMIT, "Numeric literal exceeds 128 bytes");
        return;
    }
    token->length = 1;
    switch (character) {
    case '(': token->kind = TK_LPAREN; break;
    case ')': token->kind = TK_RPAREN; break;
    case ',': token->kind = TK_COMMA; break;
    case ';': token->kind = TK_SEMICOLON; break;
    case '+': token->kind = TK_PLUS; break;
    case '-': token->kind = TK_MINUS; break;
    case '*': token->kind = TK_STAR; break;
    case '/': token->kind = TK_SLASH; break;
    case '%': token->kind = TK_PERCENT; break;
    case '=':
        token->kind = TK_EQ;
        if (parser->position < parser->length && sql[parser->position] == '=') ++parser->position;
        break;
    case '!':
        if (parser->position < parser->length && sql[parser->position] == '=') {
            ++parser->position; token->kind = TK_NE;
        } else fail(parser, ADB_ERR_PARSE, "Expected !=");
        break;
    case '<':
        token->kind = TK_LT;
        if (parser->position < parser->length && sql[parser->position] == '=') {
            ++parser->position; token->kind = TK_LE;
        } else if (parser->position < parser->length && sql[parser->position] == '>') {
            ++parser->position; token->kind = TK_NE;
        }
        break;
    case '>':
        token->kind = TK_GT;
        if (parser->position < parser->length && sql[parser->position] == '=') {
            ++parser->position; token->kind = TK_GE;
        }
        break;
    default: fail(parser, ADB_ERR_PARSE, "Unexpected character in SQL"); break;
    }
}

static bool take(Parser *parser, TokenKind kind)
{
    if (parser->failed || parser->token.kind != kind) return false;
    next(parser);
    return true;
}

static bool take_keyword(Parser *parser, const char *word)
{
    if (!keyword(parser, word) || parser->failed) return false;
    next(parser);
    return true;
}

static bool expect(Parser *parser, TokenKind kind, const char *message)
{
    return take(parser, kind) || fail(parser, ADB_ERR_PARSE, message);
}

static bool expect_keyword(Parser *parser, const char *word)
{
    if (take_keyword(parser, word)) return true;
    char message[128];
    (void)snprintf(message, sizeof message, "Expected %s", word);
    return fail(parser, ADB_ERR_PARSE, message);
}

static char *token_copy(Parser *parser)
{
    Token *token = &parser->token;
    char *text = adb_alloc(token->length + 1, 1, parser->error);
    if (!text) { parser->failed = true; return NULL; }
    memcpy(text, token->decoded ? token->decoded : parser->sql + token->start, token->length);
    text[token->length] = '\0';
    return text;
}

static char *identifier(Parser *parser)
{
    if (parser->failed || parser->token.kind != TK_NAME) {
        fail(parser, ADB_ERR_PARSE, "Expected an identifier");
        return NULL;
    }
    char *name = token_copy(parser);
    next(parser);
    return name;
}

static bool grow(Parser *parser, void **array, size_t count, size_t size, size_t maximum)
{
    if (parser->failed) return false;
    if (count > maximum || count > SIZE_MAX / size)
        return fail(parser, ADB_ERR_LIMIT, "SQL list exceeds its size limit");
    void *replacement = realloc(*array, count * size);
    if (!replacement) return fail(parser, ADB_ERR_MEMORY, "Unable to allocate SQL list");
    *array = replacement;
    return true;
}

void adb_expr_free(ADBExpr *expression)
{
    if (!expression) return;
    adb_expr_free(expression->left);
    adb_expr_free(expression->right);
    adb_value_free(&expression->literal);
    free(expression->name);
    free(expression);
}

void adb_statement_free(ADBStatement *statement)
{
    if (!statement) return;
    free(statement->table);
    free(statement->index_name);
    for (size_t i = 0; i < statement->ncolumns; ++i) adb_column_free(&statement->columns[i]);
    free(statement->columns);
    for (size_t i = 0; i < statement->ninsert_columns; ++i) free(statement->insert_columns[i]);
    free(statement->insert_columns);
    for (size_t i = 0; i < statement->nvalue_rows; ++i) {
        for (size_t j = 0; j < statement->value_counts[i]; ++j) adb_expr_free(statement->values[i][j]);
        free(statement->values[i]);
    }
    free(statement->values);
    free(statement->value_counts);
    for (size_t i = 0; i < statement->nprojections; ++i) {
        adb_expr_free(statement->projections[i].expression);
        free(statement->projections[i].alias);
    }
    free(statement->projections);
    adb_expr_free(statement->where);
    for (size_t i = 0; i < statement->nassignments; ++i) {
        free(statement->assignments[i].column);
        adb_expr_free(statement->assignments[i].value);
    }
    free(statement->assignments);
    free(statement->order_column);
    adb_statement_free(statement->inner);
    free(statement);
}

static ADBExpr *new_expression(Parser *parser, ADBExprKind kind)
{
    if (++parser->nodes > 65536u) {
        fail(parser, ADB_ERR_LIMIT, "SQL has too many expression nodes");
        return NULL;
    }
    ADBExpr *expression = adb_alloc(1, sizeof *expression, parser->error);
    if (!expression) { parser->failed = true; return NULL; }
    expression->kind = kind;
    return expression;
}

static unsigned expression_height(const ADBExpr *expression)
{
    if (!expression) return 0;
    unsigned left = expression_height(expression->left);
    unsigned right = expression_height(expression->right);
    return 1u + (left > right ? left : right);
}

static ADBExpr *parse_expression(Parser *parser, unsigned minimum);

static bool number_value(Parser *parser, ADBValue *value, bool negative)
{
    char buffer[130];
    size_t length = parser->token.length;
    if (parser->token.kind != TK_NUMBER || length >= sizeof buffer)
        return fail(parser, ADB_ERR_PARSE, "Expected a numeric literal");
    memcpy(buffer, parser->sql + parser->token.start, length);
    buffer[length] = '\0';
    errno = 0;
    if (strchr(buffer, '.') || strchr(buffer, 'e') || strchr(buffer, 'E')) {
        char *end;
        double real = strtod(buffer, &end);
        if (errno == ERANGE || *end || !isfinite(real))
            return fail(parser, ADB_ERR_TYPE, "Real literal is outside the finite double range");
        value->type = ADB_REAL;
        value->as.real = negative ? -real : real;
    } else {
        char *end;
        unsigned long long integer = strtoull(buffer, &end, 10);
        uint64_t maximum = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
        if (errno == ERANGE || *end || integer > maximum)
            return fail(parser, ADB_ERR_TYPE, "Integer literal is outside the signed 64-bit range");
        value->type = ADB_INTEGER;
        value->as.integer = negative ? (integer == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)integer) : (int64_t)integer;
    }
    next(parser);
    return !parser->failed;
}

static ADBExpr *prefix(Parser *parser)
{
    if (parser->failed) return NULL;
    if (take(parser, TK_LPAREN)) {
        ADBExpr *inside = parse_expression(parser, 1);
        if (!expect(parser, TK_RPAREN, "Expected ')' after expression")) { adb_expr_free(inside); return NULL; }
        return inside;
    }
    if (parser->token.kind == TK_PLUS || parser->token.kind == TK_MINUS || keyword(parser, "NOT")) {
        bool negative = parser->token.kind == TK_MINUS;
        bool positive = parser->token.kind == TK_PLUS;
        next(parser);
        /* Fold signed numeric literals, including INT64_MIN, without overflow. */
        if ((negative || positive) && parser->token.kind == TK_NUMBER) {
            ADBExpr *literal = new_expression(parser, EX_LITERAL);
            if (!literal) return NULL;
            if (!number_value(parser, &literal->literal, negative)) { adb_expr_free(literal); return NULL; }
            return literal;
        }
        ADBExpr *child = parse_expression(parser, positive || negative ? 7u : 3u);
        if (!child) return NULL;
        if (positive) return child;
        if (expression_height(child) >= ADB_MAX_EXPR_DEPTH) {
            adb_expr_free(child);
            fail(parser, ADB_ERR_LIMIT, "Expression tree exceeds its depth limit");
            return NULL;
        }
        ADBExpr *expression = new_expression(parser, EX_UNARY);
        if (!expression) { adb_expr_free(child); return NULL; }
        expression->op = negative ? OP_NEG : OP_NOT;
        expression->left = child;
        return expression;
    }
    if (parser->token.kind == TK_NUMBER || parser->token.kind == TK_STRING ||
        keyword(parser, "TRUE") || keyword(parser, "FALSE") || keyword(parser, "NULL")) {
        ADBExpr *expression = new_expression(parser, EX_LITERAL);
        if (!expression) return NULL;
        if (parser->token.kind == TK_NUMBER) {
            if (!number_value(parser, &expression->literal, false)) { adb_expr_free(expression); return NULL; }
        } else if (parser->token.kind == TK_STRING) {
            expression->literal.type = ADB_TEXT;
            expression->literal.as.text = token_copy(parser);
            next(parser);
        } else {
            expression->literal.type = keyword(parser, "NULL") ? ADB_NULL : ADB_BOOLEAN;
            expression->literal.as.boolean = keyword(parser, "TRUE");
            next(parser);
        }
        if (parser->failed) { adb_expr_free(expression); return NULL; }
        return expression;
    }
    if (take(parser, TK_STAR)) return new_expression(parser, EX_STAR);
    if (parser->token.kind == TK_NAME) {
        char *name = identifier(parser);
        if (!name) return NULL;
        ADBExpr *expression = new_expression(parser, EX_COLUMN);
        if (!expression) { free(name); return NULL; }
        expression->name = name;
        if (take(parser, TK_LPAREN)) {
            expression->kind = EX_FUNCTION;
            expression->left = parse_expression(parser, 1);
            if (!expression->left || !expect(parser, TK_RPAREN, "Expected ')' after function argument")) {
                adb_expr_free(expression); return NULL;
            }
        }
        return expression;
    }
    fail(parser, ADB_ERR_PARSE, "Expected an expression");
    return NULL;
}

static unsigned operator_precedence(const Parser *parser, ADBOp *operation)
{
    switch (parser->token.kind) {
    case TK_PLUS: *operation = OP_ADD; return 5;
    case TK_MINUS: *operation = OP_SUB; return 5;
    case TK_STAR: *operation = OP_MUL; return 6;
    case TK_SLASH: *operation = OP_DIV; return 6;
    case TK_PERCENT: *operation = OP_MOD; return 6;
    case TK_EQ: *operation = OP_EQ; return 4;
    case TK_NE: *operation = OP_NE; return 4;
    case TK_LT: *operation = OP_LT; return 4;
    case TK_LE: *operation = OP_LE; return 4;
    case TK_GT: *operation = OP_GT; return 4;
    case TK_GE: *operation = OP_GE; return 4;
    default: break;
    }
    if (keyword(parser, "OR")) { *operation = OP_OR; return 1; }
    if (keyword(parser, "AND")) { *operation = OP_AND; return 2; }
    if (keyword(parser, "LIKE")) { *operation = OP_LIKE; return 4; }
    if (keyword(parser, "NOT")) { *operation = OP_NOT_LIKE; return 4; }
    if (keyword(parser, "IS")) { *operation = OP_IS_NULL; return 4; }
    return 0;
}

static ADBExpr *parse_expression(Parser *parser, unsigned minimum)
{
    if (++parser->depth > ADB_MAX_EXPR_DEPTH) {
        --parser->depth;
        fail(parser, ADB_ERR_LIMIT, "Expression nesting exceeds its depth limit");
        return NULL;
    }
    ADBExpr *left = prefix(parser);
    if (!left) { --parser->depth; return NULL; }
    for (;;) {
        ADBOp operation = OP_NONE;
        unsigned precedence = operator_precedence(parser, &operation);
        if (parser->failed || precedence < minimum) break;
        next(parser);
        ADBExpr *right = NULL;
        bool unary = operation == OP_IS_NULL;
        if (unary) {
            if (take_keyword(parser, "NOT")) operation = OP_IS_NOT_NULL;
            if (!expect_keyword(parser, "NULL")) break;
        } else {
            if (operation == OP_NOT_LIKE && !expect_keyword(parser, "LIKE")) break;
            right = parse_expression(parser, precedence + 1);
            if (!right) break;
        }
        if (expression_height(left) >= ADB_MAX_EXPR_DEPTH || expression_height(right) >= ADB_MAX_EXPR_DEPTH) {
            adb_expr_free(right);
            fail(parser, ADB_ERR_LIMIT, "Expression tree exceeds its depth limit");
            break;
        }
        ADBExpr *combined = new_expression(parser, unary ? EX_UNARY : EX_BINARY);
        if (!combined) { adb_expr_free(right); break; }
        combined->op = operation;
        combined->left = left;
        combined->right = right;
        left = combined;
    }
    --parser->depth;
    if (parser->failed) { adb_expr_free(left); return NULL; }
    return left;
}

static bool unsigned_integer(Parser *parser, uint64_t *value)
{
    if (parser->token.kind != TK_NUMBER)
        return fail(parser, ADB_ERR_PARSE, "Expected a nonnegative integer");
    char buffer[130];
    size_t length = parser->token.length;
    memcpy(buffer, parser->sql + parser->token.start, length);
    buffer[length] = '\0';
    if (strchr(buffer, '.') || strchr(buffer, 'e') || strchr(buffer, 'E'))
        return fail(parser, ADB_ERR_PARSE, "Expected a nonnegative integer");
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(buffer, &end, 10);
    if (errno == ERANGE || *end) return fail(parser, ADB_ERR_LIMIT, "Integer exceeds the unsigned 64-bit range");
    *value = (uint64_t)parsed;
    next(parser);
    return !parser->failed;
}

static bool parse_column(Parser *parser, ADBColumn *column)
{
    column->name = identifier(parser);
    if (!column->name) return false;
    if (take_keyword(parser, "INTEGER") || take_keyword(parser, "INT") || take_keyword(parser, "BIGINT")) column->type = ADB_INTEGER;
    else if (take_keyword(parser, "REAL") || take_keyword(parser, "FLOAT") || take_keyword(parser, "DOUBLE")) column->type = ADB_REAL;
    else if (take_keyword(parser, "TEXT") || take_keyword(parser, "VARCHAR")) {
        column->type = ADB_TEXT;
        if (take(parser, TK_LPAREN)) {
            uint64_t width;
            if (!unsigned_integer(parser, &width)) return false;
            if (width == 0 || width > ADB_MAX_TEXT) return fail(parser, ADB_ERR_LIMIT, "Text width exceeds its size limit");
            if (!expect(parser, TK_RPAREN, "Expected ')' after text width")) return false;
        }
    } else if (take_keyword(parser, "BOOLEAN") || take_keyword(parser, "BOOL")) column->type = ADB_BOOLEAN;
    else return fail(parser, ADB_ERR_PARSE, "Expected INTEGER, REAL, TEXT, or BOOLEAN column type");
    for (;;) {
        if (take_keyword(parser, "PRIMARY")) {
            if (!expect_keyword(parser, "KEY")) return false;
            column->primary_key = column->unique = column->not_null = true;
        } else if (take_keyword(parser, "UNIQUE")) column->unique = true;
        else if (take_keyword(parser, "NOT")) {
            if (!expect_keyword(parser, "NULL")) return false;
            column->not_null = true;
        } else if (take_keyword(parser, "NULL")) {
            /* Explicit NULL is accepted unless NOT NULL was also specified. */
        } else if (take_keyword(parser, "DEFAULT")) {
            if (column->has_default) return fail(parser, ADB_ERR_PARSE, "Column has multiple DEFAULT clauses");
            bool negative = take(parser, TK_MINUS);
            bool signed_value = negative || take(parser, TK_PLUS);
            if (parser->token.kind == TK_NUMBER) {
                if (!number_value(parser, &column->default_value, negative)) return false;
            } else if (signed_value) return fail(parser, ADB_ERR_PARSE, "A sign requires a numeric default");
            else if (parser->token.kind == TK_STRING) {
                column->default_value.type = ADB_TEXT;
                column->default_value.as.text = token_copy(parser);
                next(parser);
                if (parser->failed) return false;
            } else if (keyword(parser, "TRUE") || keyword(parser, "FALSE") || keyword(parser, "NULL")) {
                column->default_value.type = keyword(parser, "NULL") ? ADB_NULL : ADB_BOOLEAN;
                column->default_value.as.boolean = keyword(parser, "TRUE");
                next(parser);
            } else return fail(parser, ADB_ERR_PARSE, "DEFAULT requires a literal value");
            column->has_default = true;
        } else break;
    }
    return !parser->failed;
}

static bool parse_create(Parser *parser, ADBStatement *statement)
{
    bool unique = take_keyword(parser, "UNIQUE");
    if (take_keyword(parser, "TABLE")) {
        if (unique) return fail(parser, ADB_ERR_PARSE, "UNIQUE applies to CREATE INDEX");
        statement->kind = ST_CREATE_TABLE;
        if (take_keyword(parser, "IF")) {
            if (!expect_keyword(parser, "NOT") || !expect_keyword(parser, "EXISTS")) return false;
            statement->if_not_exists = true;
        }
        statement->table = identifier(parser);
        if (!statement->table || !expect(parser, TK_LPAREN, "Expected '(' before column definitions")) return false;
        do {
            if (!grow(parser, (void **)&statement->columns, statement->ncolumns + 1, sizeof *statement->columns, ADB_MAX_COLUMNS)) return false;
            ADBColumn *column = &statement->columns[statement->ncolumns++];
            memset(column, 0, sizeof *column);
            if (!parse_column(parser, column)) return false;
        } while (take(parser, TK_COMMA));
        return expect(parser, TK_RPAREN, "Expected ')' after column definitions");
    }
    if (!expect_keyword(parser, "INDEX")) return false;
    statement->kind = ST_CREATE_INDEX;
    statement->unique = unique;
    if (take_keyword(parser, "IF")) {
        if (!expect_keyword(parser, "NOT") || !expect_keyword(parser, "EXISTS")) return false;
        statement->if_not_exists = true;
    }
    statement->index_name = identifier(parser);
    if (!statement->index_name || !expect_keyword(parser, "ON")) return false;
    statement->table = identifier(parser);
    if (!statement->table || !expect(parser, TK_LPAREN, "Expected '(' before indexed column")) return false;
    statement->order_column = identifier(parser);
    return statement->order_column && expect(parser, TK_RPAREN, "Expected ')' after indexed column");
}

static bool parse_drop(Parser *parser, ADBStatement *statement)
{
    if (take_keyword(parser, "TABLE")) statement->kind = ST_DROP_TABLE;
    else if (take_keyword(parser, "INDEX")) statement->kind = ST_DROP_INDEX;
    else return fail(parser, ADB_ERR_PARSE, "Expected TABLE or INDEX after DROP");
    if (take_keyword(parser, "IF")) {
        if (!expect_keyword(parser, "EXISTS")) return false;
        statement->if_exists = true;
    }
    if (statement->kind == ST_DROP_TABLE) statement->table = identifier(parser);
    else statement->index_name = identifier(parser);
    return !parser->failed;
}

static bool parse_insert(Parser *parser, ADBStatement *statement)
{
    statement->kind = ST_INSERT;
    if (!expect_keyword(parser, "INTO")) return false;
    statement->table = identifier(parser);
    if (!statement->table) return false;
    if (take(parser, TK_LPAREN)) {
        do {
            if (!grow(parser, (void **)&statement->insert_columns, statement->ninsert_columns + 1,
                      sizeof *statement->insert_columns, ADB_MAX_COLUMNS)) return false;
            char *column = identifier(parser);
            if (!column) return false;
            statement->insert_columns[statement->ninsert_columns++] = column;
        } while (take(parser, TK_COMMA));
        if (!expect(parser, TK_RPAREN, "Expected ')' after insert columns")) return false;
    }
    if (!expect_keyword(parser, "VALUES")) return false;
    do {
        if (!expect(parser, TK_LPAREN, "Expected '(' before inserted row")) return false;
        ADBExpr **row = NULL;
        size_t count = 0;
        do {
            if (!grow(parser, (void **)&row, count + 1, sizeof *row, ADB_MAX_COLUMNS)) break;
            ADBExpr *expression = parse_expression(parser, 1);
            if (!expression) break;
            row[count++] = expression;
        } while (take(parser, TK_COMMA));
        if (!parser->failed) (void)expect(parser, TK_RPAREN, "Expected ')' after inserted row");
        if (!parser->failed &&
            grow(parser, (void **)&statement->values, statement->nvalue_rows + 1, sizeof *statement->values, ADB_MAX_ROWS) &&
            grow(parser, (void **)&statement->value_counts, statement->nvalue_rows + 1, sizeof *statement->value_counts, ADB_MAX_ROWS)) {
            statement->values[statement->nvalue_rows] = row;
            statement->value_counts[statement->nvalue_rows++] = count;
        } else {
            for (size_t i = 0; i < count; ++i) adb_expr_free(row[i]);
            free(row);
            return false;
        }
    } while (take(parser, TK_COMMA));
    return !parser->failed;
}

static bool parse_select(Parser *parser, ADBStatement *statement)
{
    statement->kind = ST_SELECT;
    do {
        if (!grow(parser, (void **)&statement->projections, statement->nprojections + 1,
                  sizeof *statement->projections, ADB_MAX_COLUMNS)) return false;
        ADBProjection *projection = &statement->projections[statement->nprojections++];
        memset(projection, 0, sizeof *projection);
        projection->expression = parse_expression(parser, 1);
        if (!projection->expression) return false;
        if (take_keyword(parser, "AS")) {
            projection->alias = identifier(parser);
            if (!projection->alias) return false;
        }
    } while (take(parser, TK_COMMA));
    if (!expect_keyword(parser, "FROM")) return false;
    statement->table = identifier(parser);
    if (!statement->table) return false;
    if (take_keyword(parser, "WHERE")) {
        statement->where = parse_expression(parser, 1);
        if (!statement->where) return false;
    }
    if (take_keyword(parser, "ORDER")) {
        if (!expect_keyword(parser, "BY")) return false;
        statement->order_column = identifier(parser);
        if (!statement->order_column) return false;
        if (take_keyword(parser, "DESC")) statement->order_desc = true;
        else (void)take_keyword(parser, "ASC");
    }
    if (take_keyword(parser, "LIMIT")) {
        statement->has_limit = true;
        if (!unsigned_integer(parser, &statement->limit)) return false;
    }
    if (take_keyword(parser, "OFFSET")) {
        if (!unsigned_integer(parser, &statement->offset)) return false;
    }
    return !parser->failed;
}

static bool parse_update(Parser *parser, ADBStatement *statement)
{
    statement->kind = ST_UPDATE;
    statement->table = identifier(parser);
    if (!statement->table || !expect_keyword(parser, "SET")) return false;
    do {
        if (!grow(parser, (void **)&statement->assignments, statement->nassignments + 1,
                  sizeof *statement->assignments, ADB_MAX_COLUMNS)) return false;
        ADBAssignment *assignment = &statement->assignments[statement->nassignments++];
        memset(assignment, 0, sizeof *assignment);
        assignment->column = identifier(parser);
        if (!assignment->column || !expect(parser, TK_EQ, "Expected '=' after assignment column")) return false;
        assignment->value = parse_expression(parser, 1);
        if (!assignment->value) return false;
    } while (take(parser, TK_COMMA));
    if (take_keyword(parser, "WHERE")) {
        statement->where = parse_expression(parser, 1);
        if (!statement->where) return false;
    }
    return !parser->failed;
}

static bool parse_delete(Parser *parser, ADBStatement *statement)
{
    statement->kind = ST_DELETE;
    if (!expect_keyword(parser, "FROM")) return false;
    statement->table = identifier(parser);
    if (!statement->table) return false;
    if (take_keyword(parser, "WHERE")) {
        statement->where = parse_expression(parser, 1);
        if (!statement->where) return false;
    }
    return !parser->failed;
}

static bool parse_statement(Parser *parser, ADBStatement *statement)
{
    if (take_keyword(parser, "CREATE")) return parse_create(parser, statement);
    if (take_keyword(parser, "DROP")) return parse_drop(parser, statement);
    if (take_keyword(parser, "INSERT")) return parse_insert(parser, statement);
    if (take_keyword(parser, "SELECT")) return parse_select(parser, statement);
    if (take_keyword(parser, "UPDATE")) return parse_update(parser, statement);
    if (take_keyword(parser, "DELETE")) return parse_delete(parser, statement);
    if (take_keyword(parser, "SHOW")) {
        statement->kind = ST_SHOW_TABLES;
        return expect_keyword(parser, "TABLES");
    }
    if (take_keyword(parser, "DESCRIBE") || take_keyword(parser, "DESC")) {
        statement->kind = ST_DESCRIBE;
        statement->table = identifier(parser);
        return statement->table != NULL;
    }
    if (take_keyword(parser, "BEGIN")) {
        statement->kind = ST_BEGIN;
        (void)take_keyword(parser, "TRANSACTION");
        return !parser->failed;
    }
    if (take_keyword(parser, "COMMIT")) { statement->kind = ST_COMMIT; return true; }
    if (take_keyword(parser, "ROLLBACK")) { statement->kind = ST_ROLLBACK; return true; }
    if (take_keyword(parser, "CHECKPOINT")) { statement->kind = ST_CHECKPOINT; return true; }
    if (take_keyword(parser, "EXPLAIN")) {
        statement->kind = ST_EXPLAIN;
        if (!expect_keyword(parser, "SELECT")) return false;
        statement->inner = adb_alloc(1, sizeof *statement->inner, parser->error);
        if (!statement->inner) { parser->failed = true; return false; }
        return parse_select(parser, statement->inner);
    }
    return fail(parser, ADB_ERR_PARSE, "Expected a SQL statement");
}

ADBStatement *adb_parse(const char *sql, ADBError *error)
{
    if (error) memset(error, 0, sizeof *error);
    if (!sql) { adb_error(error, ADB_ERR_PARSE, "SQL cannot be NULL"); return NULL; }
    size_t length = 0;
    while (length <= ADB_MAX_SQL && sql[length]) ++length;
    if (length > ADB_MAX_SQL) { adb_error(error, ADB_ERR_LIMIT, "SQL exceeds one mebibyte"); return NULL; }
    if (!adb_utf8_valid(sql)) { adb_error(error, ADB_ERR_PARSE, "SQL contains invalid UTF-8"); return NULL; }
    Parser parser = {0};
    parser.sql = sql;
    parser.length = length;
    parser.error = error;
    ADBStatement *statement = adb_alloc(1, sizeof *statement, error);
    if (!statement) return NULL;
    next(&parser);
    bool success = parse_statement(&parser, statement);
    if (success && !parser.failed) {
        (void)take(&parser, TK_SEMICOLON);
        if (parser.token.kind != TK_END) fail(&parser, ADB_ERR_PARSE, "Unexpected trailing SQL; execute one statement at a time");
    }
    free(parser.token.decoded);
    if (!success || parser.failed) { adb_statement_free(statement); return NULL; }
    return statement;
}
