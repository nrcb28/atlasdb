#include "atlasdb.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static ADBResult query(ADBSession *session, const char *sql)
{
    ADBResult result = {0};
    ADBError error = {0};
    if (!adb_execute(session, sql, &result, &error)) {
        fprintf(stderr, "Query failed: %s\n[%d] %s\n", sql, error.code, error.message);
        exit(EXIT_FAILURE);
    }
    checks++;
    return result;
}

static void run(ADBSession *session, const char *sql)
{
    ADBResult result = query(session, sql);
    adb_result_free(&result);
}

static void reject(ADBSession *session, const char *sql, ADBCode expected)
{
    ADBResult result = {0};
    ADBError error = {0};
    CHECK(!adb_execute(session, sql, &result, &error));
    CHECK(error.code == expected);
    CHECK(result.cells == NULL && result.columns == NULL && result.message == NULL);
    adb_result_free(&result);
}

static char *json_query(ADBSession *session, const char *sql)
{
    ADBResult result = query(session, sql);
    char *json = adb_result_json(&result);
    CHECK(json != NULL);
    adb_result_free(&result);
    return json;
}

static void scalar_integer(ADBSession *session, const char *sql, int64_t expected)
{
    ADBResult result = query(session, sql);
    CHECK(result.nrows == 1 && result.ncolumns == 1);
    CHECK(result.cells[0].type == ADB_INTEGER && result.cells[0].as.integer == expected);
    adb_result_free(&result);
}

static void planner_numeric_consistency(ADBSession *session)
{
    run(session, "CREATE TABLE numbers (i INTEGER, r REAL, label TEXT, flag BOOLEAN)");
    run(session, "INSERT INTO numbers VALUES "
        "(-9223372036854775808,-9.223372036854776e18,'min',FALSE),"
        "(-9007199254740993,-9007199254740992.0,'neg',TRUE),"
        "(-1,-0.5,'minus',FALSE),(0,-0.0,'zero',TRUE),(1,0.5,'one',FALSE),"
        "(9007199254740992,9007199254740992.0,'exact',TRUE),"
        "(9007199254740993,9007199254740992.0,'rounded',FALSE),"
        "(9223372036854775807,9223372036854775808.0,'max',TRUE),"
        "(NULL,NULL,NULL,NULL)");
    const char *queries[] = {
        "SELECT label FROM numbers WHERE i=9007199254740992.0 ORDER BY label",
        "SELECT label FROM numbers WHERE 9007199254740992.0=i ORDER BY label",
        "SELECT label FROM numbers WHERE i=-9007199254740992.0 ORDER BY label",
        "SELECT label FROM numbers WHERE i=-9223372036854775808.0 ORDER BY label",
        "SELECT label FROM numbers WHERE i=9223372036854775808.0 ORDER BY label",
        "SELECT label FROM numbers WHERE r=9007199254740993 ORDER BY label",
        "SELECT label FROM numbers WHERE r=9007199254740992 ORDER BY label",
        "SELECT label FROM numbers WHERE r=9223372036854775807 ORDER BY label",
        "SELECT label FROM numbers WHERE r=-9223372036854775808 ORDER BY label",
        "SELECT label FROM numbers WHERE i=0.0 OR i=1.0 ORDER BY label",
        "SELECT label FROM numbers WHERE r=0 AND flag=TRUE ORDER BY label",
        "SELECT label FROM numbers WHERE flag=FALSE AND label='rounded' ORDER BY label",
        "SELECT label FROM numbers WHERE flag=TRUE ORDER BY label",
        "SELECT label FROM numbers WHERE i=NULL ORDER BY label",
        "SELECT label FROM numbers WHERE r IS NULL ORDER BY label"
    };
    enum { QUERY_COUNT = sizeof(queries) / sizeof(queries[0]) };
    char *before[QUERY_COUNT];
    for (size_t i = 0; i < QUERY_COUNT; i++) before[i] = json_query(session, queries[i]);
    run(session, "CREATE INDEX idx_integer ON numbers(i)");
    run(session, "CREATE INDEX idx_real ON numbers(r)");
    run(session, "CREATE INDEX idx_label ON numbers(label)");
    run(session, "CREATE INDEX idx_flag ON numbers(flag)");
    for (size_t i = 0; i < QUERY_COUNT; i++) {
        char *after = json_query(session, queries[i]);
        if (strcmp(before[i], after))
            fprintf(stderr, "Plan changed result: %s\nBefore: %s\nAfter: %s\n",
                    queries[i], before[i], after);
        CHECK(strcmp(before[i], after) == 0);
        free(before[i]);
        free(after);
    }
    scalar_integer(session, "SELECT COUNT(*) FROM numbers WHERE i=9007199254740992.0", 1);
    scalar_integer(session, "SELECT COUNT(*) FROM numbers WHERE r=9007199254740993", 0);
    scalar_integer(session, "SELECT COUNT(*) FROM numbers WHERE r=9007199254740992", 2);
    scalar_integer(session, "SELECT COUNT(*) FROM numbers WHERE i=9223372036854775808.0", 0);
    scalar_integer(session, "SELECT COUNT(*) FROM numbers WHERE r=-9223372036854775808", 1);
    ADBResult result = query(session,
        "EXPLAIN SELECT label FROM numbers WHERE i=9007199254740992.0");
    CHECK(result.nrows == 1 && result.cells[0].type == ADB_TEXT);
    CHECK(strcmp(result.cells[0].as.text, "INDEX LOOKUP") == 0);
    adb_result_free(&result);
}

static void mutation_and_ordering(ADBSession *session)
{
    run(session, "CREATE TABLE swaps (id INTEGER PRIMARY KEY,a INTEGER,b INTEGER,txt TEXT UNIQUE,rank INTEGER)");
    run(session, "INSERT INTO swaps VALUES "
        "(1,10,20,'z',2),(2,30,40,'a',1),(3,50,60,'b',1),(4,70,80,NULL,NULL)");
    run(session, "UPDATE swaps SET a=b,b=a WHERE id=1");
    scalar_integer(session, "SELECT a FROM swaps WHERE id=1", 20);
    scalar_integer(session, "SELECT b FROM swaps WHERE id=1", 10);
    run(session, "UPDATE swaps SET id=5-id");
    scalar_integer(session, "SELECT a FROM swaps WHERE id=4", 20);
    scalar_integer(session, "SELECT a FROM swaps WHERE id=1", 70);
    char *baseline = json_query(session, "SELECT * FROM swaps ORDER BY id");
    reject(session, "UPDATE swaps SET txt='duplicate'", ADB_ERR_CONSTRAINT);
    reject(session, "UPDATE swaps SET a=9223372036854775807+a", ADB_ERR_TYPE);
    char *after = json_query(session, "SELECT * FROM swaps ORDER BY id");
    CHECK(strcmp(baseline, after) == 0);
    free(baseline);
    free(after);

    ADBResult result = query(session, "SELECT id FROM swaps ORDER BY rank");
    CHECK(result.nrows == 4);
    CHECK(result.cells[0].as.integer == 1);
    CHECK(result.cells[1].as.integer == 3);
    CHECK(result.cells[2].as.integer == 2);
    CHECK(result.cells[3].as.integer == 4);
    adb_result_free(&result);
    result = query(session, "SELECT a AS score,id FROM swaps ORDER BY score DESC LIMIT 2 OFFSET 1");
    CHECK(result.nrows == 2 && result.ncolumns == 2);
    CHECK(result.cells[0].as.integer == 50 && result.cells[1].as.integer == 2);
    CHECK(result.cells[2].as.integer == 30 && result.cells[3].as.integer == 3);
    adb_result_free(&result);
    result = query(session, "SELECT id FROM swaps ORDER BY id LIMIT 9223372036854775807 OFFSET 9223372036854775807");
    CHECK(result.nrows == 0);
    adb_result_free(&result);

    for (int i = 0; i < 50; i++) {
        run(session, "DELETE FROM swaps WHERE id=2 OR id=4");
        run(session, "INSERT INTO swaps VALUES (2,20,2,'reused-2',1),(4,40,4,'reused-4',2)");
        scalar_integer(session, "SELECT COUNT(*) FROM swaps WHERE id=4", 1);
    }
    run(session, "DELETE FROM swaps");
    run(session, "INSERT INTO swaps VALUES (1,1,1,'after clear',NULL)");
    scalar_integer(session, "SELECT COUNT(*) FROM swaps", 1);
}

static void aggregates_and_overflow(ADBSession *session)
{
    run(session, "CREATE TABLE aggregates (i INTEGER,r REAL)");
    run(session, "INSERT INTO aggregates VALUES (9223372036854775807,1e308),(9223372036854775807,1e308)");
    reject(session, "SELECT SUM(i) FROM aggregates", ADB_ERR_TYPE);
    reject(session, "SELECT SUM(r) FROM aggregates", ADB_ERR_TYPE);
    ADBResult result = query(session, "SELECT AVG(r),MIN(i),MAX(i) FROM aggregates");
    CHECK(result.nrows == 1 && result.ncolumns == 3);
    CHECK(result.cells[0].type == ADB_REAL && isfinite(result.cells[0].as.real));
    CHECK(result.cells[0].as.real == 1e308);
    CHECK(result.cells[1].as.integer == INT64_MAX && result.cells[2].as.integer == INT64_MAX);
    adb_result_free(&result);
    run(session, "DELETE FROM aggregates");
    run(session, "INSERT INTO aggregates VALUES (-9223372036854775808,-1e308),(9223372036854775807,1e308),(NULL,NULL)");
    scalar_integer(session, "SELECT SUM(i) FROM aggregates", -1);
    scalar_integer(session, "SELECT COUNT(*) FROM aggregates", 3);
    scalar_integer(session, "SELECT COUNT(i) FROM aggregates", 2);
    result = query(session, "SELECT AVG(i),AVG(r),SUM(r) FROM aggregates");
    CHECK(result.nrows == 1 && result.ncolumns == 3);
    CHECK(result.cells[0].type == ADB_REAL && result.cells[0].as.real == -0.5);
    CHECK(result.cells[1].type == ADB_REAL && result.cells[1].as.real == 0.0);
    CHECK(result.cells[2].type == ADB_REAL && result.cells[2].as.real == 0.0);
    adb_result_free(&result);
    reject(session, "SELECT ABS(i) FROM aggregates", ADB_ERR_TYPE);
    reject(session, "SELECT -i FROM aggregates", ADB_ERR_TYPE);
    reject(session, "SELECT i/-1 FROM aggregates", ADB_ERR_TYPE);
    result = query(session, "SELECT i%-1 FROM aggregates ORDER BY i");
    CHECK(result.nrows == 3 && result.cells[0].type == ADB_NULL);
    CHECK(result.cells[1].type == ADB_INTEGER && result.cells[1].as.integer == 0);
    CHECK(result.cells[2].type == ADB_INTEGER && result.cells[2].as.integer == 0);
    adb_result_free(&result);
}

int main(void)
{
    ADBError error = {0};
    ADBDatabase *database = adb_open(":memory:", &error);
    CHECK(database != NULL);
    ADBSession *session = adb_session_create(database, &error);
    CHECK(session != NULL);
    planner_numeric_consistency(session);
    mutation_and_ordering(session);
    aggregates_and_overflow(session);
    adb_session_destroy(session);
    adb_close(database);
    printf("engine edges: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
