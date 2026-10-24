#define _POSIX_C_SOURCE 200809L
#include "atlasdb.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static void check_at(bool condition, const char *expression, int line) {
    checks++;
    if (!condition) { fprintf(stderr, "FAIL test_atlas.c:%d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) check_at((x), #x, __LINE__)

static ADBDatabase *open_db(const char *path) {
    ADBError error = {0};
    ADBDatabase *db = adb_open(path, &error);
    if (!db) { fprintf(stderr, "open %s: %s\n", path, error.message); exit(1); }
    return db;
}
static ADBSession *session(ADBDatabase *db) {
    ADBError error = {0};
    ADBSession *s = adb_session_create(db, &error);
    CHECK(s != NULL);
    return s;
}
static ADBResult query(ADBSession *s, const char *sql) {
    ADBResult r = {0}; ADBError error = {0};
    if (!adb_execute(s, sql, &r, &error)) {
        fprintf(stderr, "SQL failed [%d] at %zu: %s\n%s\n", (int)error.code, error.offset, error.message, sql);
        exit(1);
    }
    checks++;
    return r;
}
static void run(ADBSession *s, const char *sql) {
    ADBResult r = query(s, sql); adb_result_free(&r);
}
static void reject(ADBSession *s, const char *sql, ADBCode code) {
    ADBResult r = {0}; ADBError error = {0};
    bool ok = adb_execute(s, sql, &r, &error);
    if (ok) fprintf(stderr, "Unexpected success: %s\n", sql);
    CHECK(!ok);
    if (code != ADB_OK && error.code != code)
        fprintf(stderr, "Expected error %d, got %d: %s\n", (int)code, (int)error.code, error.message);
    CHECK(code == ADB_OK || error.code == code);
    CHECK(error.message[0] != '\0');
    adb_result_free(&r);
}
static const ADBValue *cell(const ADBResult *r, size_t row, size_t col) {
    CHECK(row < r->nrows && col < r->ncolumns);
    return &r->cells[row * r->ncolumns + col];
}
static double number(const ADBValue *v) {
    CHECK(v->type == ADB_INTEGER || v->type == ADB_REAL);
    return v->type == ADB_INTEGER ? (double)v->as.integer : v->as.real;
}
static void scalar(ADBSession *s, const char *sql, double expected) {
    ADBResult r = query(s, sql);
    CHECK(r.nrows == 1 && r.ncolumns == 1);
    CHECK(fabs(number(cell(&r, 0, 0)) - expected) < 1e-9);
    adb_result_free(&r);
}
static void text_is(const ADBValue *v, const char *expected) {
    CHECK(v->type == ADB_TEXT);
    CHECK(strcmp(v->as.text, expected) == 0);
}
static void close_db(ADBDatabase *db, ADBSession *s) {
    adb_session_destroy(s); adb_close(db);
}

static void test_crud(void) {
    ADBDatabase *db = open_db(":memory:"); ADBSession *s = session(db);
    run(s, "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE, score REAL DEFAULT 1.5, active BOOLEAN DEFAULT TRUE)");
    run(s, "INSERT INTO people (id,name,email) VALUES (1,'Ana','a@example.test'), (2,'O''Brien','b@example.test'), (3,'José',NULL)");
    ADBResult r = query(s, "SELECT id,name,score,active FROM people ORDER BY id DESC LIMIT 2 OFFSET 1");
    CHECK(r.nrows == 2 && r.ncolumns == 4);
    CHECK(number(cell(&r,0,0)) == 2); text_is(cell(&r,0,1), "O'Brien");
    CHECK(number(cell(&r,1,2)) == 1.5);
    CHECK(cell(&r,1,3)->type == ADB_BOOLEAN && cell(&r,1,3)->as.boolean);
    adb_result_free(&r);
    scalar(s, "SELECT COUNT(*) FROM people", 3);
    scalar(s, "SELECT COUNT(email) FROM people", 2);
    scalar(s, "SELECT SUM(score) FROM people", 4.5);
    scalar(s, "SELECT AVG(score) FROM people", 1.5);
    scalar(s, "SELECT MIN(id) FROM people", 1);
    scalar(s, "SELECT MAX(id) FROM people", 3);
    run(s, "UPDATE people SET score=score+2, active=FALSE WHERE id=2");
    scalar(s, "SELECT score FROM people WHERE id=2", 3.5);
    r = query(s, "DELETE FROM people WHERE id=3"); CHECK(r.affected == 1); adb_result_free(&r);
    scalar(s, "SELECT COUNT(*) FROM people", 2);
    r = query(s, "SELECT * FROM people WHERE id=999"); CHECK(r.nrows == 0 && r.ncolumns == 5); adb_result_free(&r);
    r = query(s, "SHOW TABLES"); CHECK(r.nrows == 1); text_is(cell(&r,0,0),"people"); adb_result_free(&r);
    r = query(s, "DESCRIBE people"); CHECK(r.nrows == 5); adb_result_free(&r);
    run(s, "DROP TABLE people"); run(s, "DROP TABLE IF EXISTS people");
    close_db(db,s);
}

static void test_constraints_and_atomicity(void) {
    ADBDatabase *db = open_db(":memory:"); ADBSession *s = session(db);
    run(s, "CREATE TABLE items (id INT PRIMARY KEY, label TEXT UNIQUE, amount INTEGER NOT NULL DEFAULT 7)");
    run(s, "INSERT INTO items (id,label) VALUES (1,'one'),(2,NULL),(3,NULL)");
    reject(s,"INSERT INTO items VALUES (4,'four',4),(1,'duplicate',5)",ADB_ERR_CONSTRAINT);
    scalar(s,"SELECT COUNT(*) FROM items",3);
    reject(s,"UPDATE items SET id=1 WHERE id=2",ADB_ERR_CONSTRAINT);
    scalar(s,"SELECT id FROM items WHERE id=2",2);
    reject(s,"UPDATE items SET amount=NULL",ADB_ERR_CONSTRAINT);
    scalar(s,"SELECT SUM(amount) FROM items",21);
    reject(s,"INSERT INTO items VALUES (NULL,'bad',0)",ADB_ERR_CONSTRAINT);
    reject(s,"INSERT INTO items VALUES (5,'one',1)",ADB_ERR_CONSTRAINT);
    reject(s,"INSERT INTO items VALUES (5,'five','wrong type')",ADB_ERR_TYPE);
    reject(s,"CREATE TABLE bad (x INT,x TEXT)",ADB_ERR_SCHEMA);
    reject(s,"CREATE TABLE items (x INT)",ADB_ERR_SCHEMA);
    reject(s,"SELECT absent FROM items WHERE id=1",ADB_ERR_SCHEMA);
    reject(s,"SELECT * FROM nowhere",ADB_ERR_SCHEMA);
    reject(s,"INSERT INTO items (absent) VALUES (1)",ADB_ERR_SCHEMA);
    reject(s,"SELECT * FROM items; DROP TABLE items",ADB_ERR_PARSE);
    reject(s,"SELECT 'unterminated FROM items",ADB_ERR_PARSE);
    scalar(s,"SELECT COUNT(*) FROM items",3);
    close_db(db,s);
}

static void test_expressions_and_nulls(void) {
    ADBDatabase *db = open_db(":memory:"); ADBSession *s = session(db);
    run(s,"CREATE TABLE metrics (id INT PRIMARY KEY,a INTEGER,b INTEGER,label TEXT,flag BOOLEAN)");
    run(s,"INSERT INTO metrics VALUES (1,2,3,'alpha',TRUE),(2,4,NULL,'beta',FALSE),(3,NULL,5,'alpine',NULL)");
    scalar(s,"SELECT a+b*2 AS calculated FROM metrics WHERE id=1",8);
    scalar(s,"SELECT (a+b)*2 FROM metrics WHERE id=1",10);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE a IS NULL",1);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE a IS NOT NULL",2);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE a=NULL",0);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE NOT (a=2 OR id=3)",1);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE (a>=2 AND a<5) OR id=3",3);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE label LIKE 'al%'",2);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE label NOT LIKE 'al%'",1);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE label LIKE '_eta'",1);
    scalar(s,"SELECT COUNT(*) FROM metrics WHERE flag=TRUE",1);
    ADBResult r = query(s,"SELECT a+b FROM metrics WHERE id=2"); CHECK(cell(&r,0,0)->type == ADB_NULL); adb_result_free(&r);
    r = query(s,"SELECT SUM(a),AVG(a),MIN(a),MAX(a),COUNT(a) FROM metrics WHERE id=999");
    CHECK(r.nrows == 1); CHECK(cell(&r,0,0)->type == ADB_NULL); CHECK(cell(&r,0,1)->type == ADB_NULL);
    CHECK(cell(&r,0,2)->type == ADB_NULL); CHECK(cell(&r,0,3)->type == ADB_NULL); CHECK(number(cell(&r,0,4)) == 0); adb_result_free(&r);
    reject(s,"SELECT a,COUNT(*) FROM metrics",ADB_ERR_SCHEMA);
    reject(s,"SELECT missing FROM metrics WHERE id=999",ADB_ERR_SCHEMA);
    r = query(s,"SELECT label AS title FROM metrics ORDER BY id LIMIT 0"); CHECK(r.nrows == 0); CHECK(strcmp(r.columns[0],"title") == 0); adb_result_free(&r);
    run(s,"CREATE TABLE \"quoted table\" (\"column name\" TEXT)");
    run(s,"INSERT INTO \"quoted table\" VALUES ('line\nquote\"\\tab\t')");
    r = query(s,"SELECT * FROM \"quoted table\"");
    char *json = adb_result_json(&r); CHECK(json != NULL); CHECK(strstr(json,"\\n") != NULL); CHECK(strstr(json,"\\\"") != NULL); CHECK(strstr(json,"\\\\") != NULL); free(json); adb_result_free(&r);
    close_db(db,s);
}

static void test_indexes(void) {
    ADBDatabase *db = open_db(":memory:"); ADBSession *s = session(db);
    run(s,"CREATE TABLE indexed (id INT PRIMARY KEY, tag TEXT, value INTEGER)");
    run(s,"INSERT INTO indexed VALUES (1,'x',10),(2,'x',20),(3,'y',30),(4,NULL,40)");
    run(s,"CREATE INDEX tag_idx ON indexed(tag)");
    scalar(s,"SELECT SUM(value) FROM indexed WHERE tag='x'",30);
    ADBResult r = query(s,"EXPLAIN SELECT * FROM indexed WHERE tag='x'");
    CHECK(r.nrows >= 1 && r.ncolumns >= 1); adb_result_free(&r);
    run(s,"UPDATE indexed SET tag='z' WHERE id=2");
    scalar(s,"SELECT COUNT(*) FROM indexed WHERE tag='x'",1);
    scalar(s,"SELECT value FROM indexed WHERE tag='z'",20);
    run(s,"DELETE FROM indexed WHERE id=1"); scalar(s,"SELECT COUNT(*) FROM indexed WHERE tag='x'",0);
    run(s,"DROP INDEX tag_idx"); scalar(s,"SELECT value FROM indexed WHERE tag='z'",20);
    run(s,"CREATE UNIQUE INDEX unique_tag ON indexed(tag)");
    reject(s,"INSERT INTO indexed VALUES (5,'z',50)",ADB_ERR_CONSTRAINT);
    run(s,"INSERT INTO indexed VALUES (6,NULL,60)");
    scalar(s,"SELECT COUNT(*) FROM indexed WHERE tag IS NULL",2);
    close_db(db,s);
}

static void test_transactions(void) {
    ADBDatabase *db = open_db(":memory:"); ADBSession *a = session(db); ADBSession *b = session(db);
    run(a,"CREATE TABLE account (id INT PRIMARY KEY, balance INTEGER)");
    run(a,"INSERT INTO account VALUES (1,100)");
    reject(a,"COMMIT",ADB_ERR_TRANSACTION); reject(a,"ROLLBACK",ADB_ERR_TRANSACTION);
    run(a,"BEGIN"); CHECK(adb_session_in_transaction(a)); reject(a,"BEGIN",ADB_ERR_TRANSACTION);
    run(a,"UPDATE account SET balance=200 WHERE id=1");
    scalar(a,"SELECT balance FROM account WHERE id=1",200); scalar(b,"SELECT balance FROM account WHERE id=1",100);
    run(a,"ROLLBACK"); CHECK(!adb_session_in_transaction(a)); scalar(a,"SELECT balance FROM account WHERE id=1",100);
    run(a,"BEGIN"); run(b,"BEGIN");
    run(a,"UPDATE account SET balance=150 WHERE id=1"); run(a,"COMMIT");
    scalar(b,"SELECT balance FROM account WHERE id=1",100);
    run(b,"UPDATE account SET balance=175 WHERE id=1");
    reject(b,"COMMIT",ADB_ERR_CONFLICT); CHECK(adb_session_in_transaction(b)); run(b,"ROLLBACK");
    scalar(b,"SELECT balance FROM account WHERE id=1",150);
    run(a,"BEGIN"); run(a,"INSERT INTO account VALUES (2,20)");
    reject(a,"INSERT INTO account VALUES (3,30),(2,999)",ADB_ERR_CONSTRAINT);
    scalar(a,"SELECT COUNT(*) FROM account",2); run(a,"COMMIT"); scalar(b,"SELECT COUNT(*) FROM account",2);
    run(a,"BEGIN"); run(b,"UPDATE account SET balance=balance+1 WHERE id=1");
    run(a,"COMMIT"); /* read-only snapshot commits despite a concurrent write */
    run(a,"BEGIN"); run(a,"INSERT INTO account VALUES (9,9)"); adb_session_destroy(a);
    scalar(b,"SELECT COUNT(*) FROM account WHERE id=9",0);
    adb_session_destroy(b); adb_close(db);
}

static void remove_directory(const char *path) {
    DIR *d = opendir(path); CHECK(d != NULL); struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char file[1024]; int n = snprintf(file,sizeof(file),"%s/%s",path,entry->d_name);
        CHECK(n > 0 && (size_t)n < sizeof(file)); CHECK(unlink(file) == 0);
    }
    CHECK(closedir(d) == 0); CHECK(rmdir(path) == 0);
}
static void test_persistence(void) {
    char path[] = "/tmp/atlasdb-test-XXXXXX"; CHECK(mkdtemp(path) != NULL);
    ADBDatabase *db = open_db(path); ADBSession *s = session(db);
    run(s,"CREATE TABLE durable (id INT PRIMARY KEY, text TEXT DEFAULT 'saved')");
    run(s,"CREATE INDEX durable_text ON durable(text)");
    run(s,"INSERT INTO durable (id) VALUES (1),(2)");
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        ADBError error = {0}; ADBDatabase *other = adb_open(path,&error);
        if (other) { adb_close(other); _exit(2); }
        _exit(error.code == ADB_ERR_IO ? 0 : 3);
    }
    int status; CHECK(waitpid(child,&status,0) == child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    run(s,"BEGIN"); run(s,"INSERT INTO durable VALUES (99,'not committed')");
    close_db(db,s);
    db = open_db(path); s = session(db); scalar(s,"SELECT COUNT(*) FROM durable",2);
    ADBResult r = query(s,"SELECT text FROM durable WHERE id=1"); text_is(cell(&r,0,0),"saved"); adb_result_free(&r);
    ADBError error = {0}; CHECK(adb_checkpoint(db,&error));
    run(s,"INSERT INTO durable VALUES (3,'after checkpoint')"); close_db(db,s);
    db = open_db(path); s = session(db); scalar(s,"SELECT COUNT(*) FROM durable",3);
    run(s,"CHECKPOINT"); close_db(db,s);
    db = open_db(path); s = session(db); scalar(s,"SELECT COUNT(*) FROM durable",3); close_db(db,s);
    remove_directory(path);
}

int main(void) {
    test_crud(); test_constraints_and_atomicity(); test_expressions_and_nulls();
    test_indexes(); test_transactions(); test_persistence();
    printf("AtlasDB C API: %u checks passed\n", checks);
    return 0;
}
