#include "internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;

static void check(bool condition, const char *description)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "parser check failed: %s\n", description);
        exit(EXIT_FAILURE);
    }
}

static ADBStatement *accept(const char *sql, ADBStatementKind expected)
{
    ADBError error;
    ADBStatement *statement = adb_parse(sql, &error);
    if (!statement) fprintf(stderr, "SQL: %s\nerror at %zu: %s\n", sql, error.offset, error.message);
    check(statement != NULL, "valid statement accepted");
    check(statement->kind == expected, "statement kind matches");
    check(error.code == ADB_OK, "success clears error");
    return statement;
}

static void reject(const char *sql)
{
    ADBError error;
    ADBStatement *statement = adb_parse(sql, &error);
    if (statement) fprintf(stderr, "unexpectedly accepted SQL: %s\n", sql);
    check(statement == NULL, "invalid statement rejected");
    check(error.code != ADB_OK, "rejection reports an error");
    check(error.message[0] != '\0', "rejection describes error");
    check(error.offset <= (sql ? strlen(sql) : 0), "error offset belongs to SQL");
    adb_statement_free(statement);
}

static void grammar_checks(void)
{
    ADBStatement *statement = accept(
        "-- schema\nCREATE TABLE IF NOT EXISTS \"order items\" ("
        "id INT PRIMARY KEY, description VARCHAR(100) NOT NULL DEFAULT 'can''t; stop',"
        "price REAL DEFAULT -3.5e2, active BOOL DEFAULT TRUE, optional TEXT DEFAULT NULL); /* end */",
        ST_CREATE_TABLE);
    check(statement->if_not_exists && statement->ncolumns == 5, "schema flags and columns");
    check(strcmp(statement->table, "order items") == 0, "quoted identifier");
    check(statement->columns[0].primary_key && statement->columns[0].not_null && statement->columns[0].unique, "primary key implies constraints");
    check(strcmp(statement->columns[1].default_value.as.text, "can't; stop") == 0, "escaped string default");
    check(statement->columns[2].default_value.as.real == -350.0, "signed exponent default");
    adb_statement_free(statement);

    statement = accept("INSERT INTO t (a, b) VALUES (1, 'a'), (-9223372036854775808, 'b');", ST_INSERT);
    check(statement->nvalue_rows == 2 && statement->value_counts[1] == 2, "multiple inserted rows");
    check(statement->values[1][0]->literal.as.integer == INT64_MIN, "minimum signed integer");
    adb_statement_free(statement);

    statement = accept("SELECT a + 2 * 3 AS total, COUNT(*), ABS(-2) FROM t WHERE NOT a = 3 OR b IS NOT NULL AND b NOT LIKE 'a_%' ORDER BY a DESC LIMIT 10 OFFSET 2;", ST_SELECT);
    check(statement->nprojections == 3 && statement->where->op == OP_OR, "SELECT projections and OR precedence");
    check(statement->projections[0].expression->op == OP_ADD && statement->projections[0].expression->right->op == OP_MUL, "multiplication binds tighter than addition");
    check(statement->where->left->op == OP_NOT && statement->where->left->left->op == OP_EQ, "NOT binds below comparison");
    check(statement->where->right->op == OP_AND, "AND binds tighter than OR");
    check(statement->where->right->left->op == OP_IS_NOT_NULL, "IS NOT NULL unary expression");
    check(statement->where->right->right->op == OP_NOT_LIKE, "NOT LIKE binary expression");
    check(statement->order_desc && statement->has_limit && statement->limit == 10 && statement->offset == 2, "ordering and slicing");
    adb_statement_free(statement);

    statement = accept("SELECT 9 - 4 - 2 FROM t", ST_SELECT);
    check(statement->projections[0].expression->left->op == OP_SUB, "subtraction associates to the left");
    adb_statement_free(statement);
    statement = accept("UPDATE t SET a=a+1, b=LOWER(b) WHERE a>=0 AND a<>8", ST_UPDATE);
    check(statement->nassignments == 2 && statement->where->op == OP_AND, "UPDATE expressions");
    adb_statement_free(statement);
    statement = accept("DELETE FROM t WHERE a!=1", ST_DELETE);
    adb_statement_free(statement);
    statement = accept("CREATE UNIQUE INDEX IF NOT EXISTS lookup ON t(a)", ST_CREATE_INDEX);
    check(statement->unique && statement->if_not_exists && strcmp(statement->order_column, "a") == 0, "index metadata");
    adb_statement_free(statement);
    statement = accept("DROP INDEX IF EXISTS lookup", ST_DROP_INDEX);
    check(statement->if_exists, "conditional index drop");
    adb_statement_free(statement);
    statement = accept("DROP TABLE IF EXISTS t", ST_DROP_TABLE);
    adb_statement_free(statement);
    statement = accept("SHOW TABLES", ST_SHOW_TABLES);
    adb_statement_free(statement);
    statement = accept("DESCRIBE `a``b`", ST_DESCRIBE);
    check(strcmp(statement->table, "a`b") == 0, "escaped quoted identifier");
    adb_statement_free(statement);
    statement = accept("BEGIN TRANSACTION;", ST_BEGIN);
    adb_statement_free(statement);
    statement = accept("COMMIT", ST_COMMIT);
    adb_statement_free(statement);
    statement = accept("ROLLBACK", ST_ROLLBACK);
    adb_statement_free(statement);
    statement = accept("CHECKPOINT", ST_CHECKPOINT);
    adb_statement_free(statement);
    statement = accept("EXPLAIN SELECT * FROM t WHERE a=1", ST_EXPLAIN);
    check(statement->inner && statement->inner->kind == ST_SELECT, "EXPLAIN wraps SELECT");
    adb_statement_free(statement);
    statement = accept("SELECT .5, 1., 1E+2, -0.0, '', NULL, TRUE, FALSE FROM t OFFSET 0", ST_SELECT);
    check(statement->nprojections == 8, "literal forms");
    adb_statement_free(statement);
    statement = accept("SELECT 'ni" "\xc3\xb1" "o \xf0\x9f\x8c\x8d' FROM \"a" "\xc3\xb1" "o\"", ST_SELECT);
    check(strcmp(statement->projections[0].expression->literal.as.text, "ni" "\xc3\xb1" "o \xf0\x9f\x8c\x8d") == 0, "UTF-8 literals preserved");
    adb_statement_free(statement);
}

static void rejection_checks(void)
{
    const char *invalid[] = {
        "", "  -- only a comment", "/* unclosed", "SELECT * FROM t; SELECT * FROM t", "SHOW TABLES;;",
        "SELECT * FROM", "SELECT *", "SELECT FROM t", "SELECT a, FROM t", "SELECT a FROM t WHERE",
        "SELECT a FROM t LIMIT -1", "SELECT a FROM t LIMIT 1.0", "SELECT a FROM t LIMIT 18446744073709551616",
        "SELECT a FROM t ORDER a", "SELECT a FROM t WHERE a IS 3", "SELECT a FROM t WHERE a NOT IN (1)",
        "SELECT 'unclosed FROM t", "SELECT \"unclosed FROM t", "SELECT \"\" FROM t", "SELECT a ! 3 FROM t",
        "SELECT 1e FROM t", "SELECT 1e+ FROM t", "SELECT 1e999 FROM t", "SELECT 9223372036854775808 FROM t",
        "SELECT -9223372036854775809 FROM t", "CREATE TABLE t()", "CREATE TABLE t(a WHAT)",
        "CREATE TABLE t(a TEXT DEFAULT 1+2)", "CREATE TABLE t(a TEXT DEFAULT TRUE DEFAULT FALSE)",
        "CREATE TABLE t(a TEXT DEFAULT +TRUE)", "CREATE TABLE t(a VARCHAR(0))", "CREATE TABLE t(a VARCHAR(1048577))",
        "INSERT INTO t VALUES ()", "INSERT INTO t(a,) VALUES (1)", "INSERT INTO t VALUES (1),", "UPDATE t SET a=",
        "UPDATE t SET a 1", "DELETE t", "CREATE INDEX i ON t(a,b)", "DROP THING t", "EXPLAIN DELETE FROM t",
        "BEGIN TRANSACTION EXTRA", "SELECT COUNT() FROM t", "SELECT SUM(a,b) FROM t", "SELECT a FROM t WHERE a IN (1,2)"
    };
    reject(NULL);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) reject(invalid[i]);
    reject("SELECT '\xc0\xaf' FROM t"); /* overlong slash */
    reject("SELECT '\xe2\x82' FROM t"); /* truncated sequence */
    reject("SELECT '\xed\xa0\x80' FROM t"); /* UTF-16 surrogate */
    reject("SELECT '\xf4\x90\x80\x80' FROM t"); /* beyond Unicode */
    reject("-- invalid comment \x80");
    char identifier[400];
    strcpy(identifier, "SELECT ");
    memset(identifier + 7, 'a', 256);
    strcpy(identifier + 263, " FROM t");
    reject(identifier);

    char expression[5000];
    strcpy(expression, "SELECT ");
    size_t offset = strlen(expression);
    for (size_t i = 0; i < 1000; ++i) { expression[offset++] = '1'; expression[offset++] = '+'; }
    strcpy(expression + offset, "1 FROM t");
    reject(expression);
    strcpy(expression, "SELECT ");
    offset = strlen(expression);
    for (size_t i = 0; i < 300; ++i) expression[offset++] = '(';
    expression[offset++] = '1';
    for (size_t i = 0; i < 300; ++i) expression[offset++] = ')';
    strcpy(expression + offset, " FROM t");
    reject(expression);

    char *oversized = malloc(ADB_MAX_SQL + 2u);
    check(oversized != NULL, "allocate oversized SQL test");
    memset(oversized, ' ', ADB_MAX_SQL + 1u);
    oversized[ADB_MAX_SQL + 1u] = '\0';
    reject(oversized);
    free(oversized);
}

static unsigned random_state = 0xc0ffeeu;
static unsigned random_next(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void malformed_stress(void)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_()'\"`;,=<>!+-*/% \n\t";
    char sql[256];
    for (size_t i = 0; i < 12000; ++i) {
        size_t length = random_next() % (sizeof sql - 1);
        for (size_t j = 0; j < length; ++j) sql[j] = alphabet[random_next() % (sizeof alphabet - 1)];
        sql[length] = '\0';
        ADBError error;
        ADBStatement *statement = adb_parse(sql, &error);
        check(statement != NULL || error.code != ADB_OK, "malformed SQL reports status");
        adb_statement_free(statement);
    }
    static const char *seeds[] = {
        "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT DEFAULT 'x', value REAL)",
        "INSERT INTO t VALUES (1, 'hello', .5), (2, 'world', -99.0)",
        "SELECT id+2*3, LOWER(name), COUNT(*) FROM t WHERE NOT id=1 OR name LIKE 'h%' ORDER BY id DESC LIMIT 20",
        "UPDATE t SET name=UPPER(name), value=value+1 WHERE id>=1",
        "CREATE UNIQUE INDEX idx ON t(name)"
    };
    for (size_t i = 0; i < 6000; ++i) {
        const char *seed = seeds[random_next() % (sizeof seeds / sizeof seeds[0])];
        size_t length = strlen(seed);
        memcpy(sql, seed, length + 1);
        unsigned changes = random_next() % 6u;
        for (unsigned j = 0; j < changes; ++j) sql[random_next() % length] = alphabet[random_next() % (sizeof alphabet - 1)];
        ADBError error;
        ADBStatement *statement = adb_parse(sql, &error);
        check(statement != NULL || error.code != ADB_OK, "mutated SQL reports status");
        adb_statement_free(statement);
    }
}

int main(void)
{
    grammar_checks();
    rejection_checks();
    malformed_stress();
    printf("parser: %zu checks passed (18,000 deterministic stress inputs)\n", checks);
    return EXIT_SUCCESS;
}
