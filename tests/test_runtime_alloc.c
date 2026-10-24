/* Runtime atomicity under a failed allocation. GNU ld --wrap observes this
 * executable and the static AtlasDB library, not shared libc internals. */
#include "atlasdb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

static long outstanding;
static bool tracking;
static bool denied;
static size_t allocation_index;
static size_t failure_index = SIZE_MAX;
static const char *current_case = "setup";
static size_t current_failure;
static size_t checks;

static void check(bool condition, const char *description)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "runtime allocation test: %s, allocation %zu: %s\n",
                current_case, current_failure, description);
        exit(EXIT_FAILURE);
    }
}

static bool deny_allocation(void)
{
    if (!tracking) return false;
    if (allocation_index++ != failure_index) return false;
    denied = true;
    return true;
}

void *__wrap_malloc(size_t size)
{
    if (deny_allocation()) return NULL;
    void *pointer = __real_malloc(size);
    if (pointer) ++outstanding;
    return pointer;
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (deny_allocation()) return NULL;
    void *pointer = __real_calloc(count, size);
    if (pointer) ++outstanding;
    return pointer;
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (deny_allocation()) return NULL;
    if (size == 0) {
        if (pointer) --outstanding;
        __real_free(pointer);
        return NULL;
    }
    void *replacement = __real_realloc(pointer, size);
    if (replacement && !pointer) ++outstanding;
    return replacement;
}

void __wrap_free(void *pointer)
{
    if (pointer) --outstanding;
    __real_free(pointer);
}

static void inject(size_t index)
{
    failure_index = index;
    allocation_index = 0;
    denied = false;
    tracking = true;
}

static size_t stop_injection(void)
{
    tracking = false;
    return allocation_index;
}

static void execute_ok(ADBSession *session, const char *sql)
{
    ADBError error;
    ADBResult result;
    bool ok = adb_execute(session, sql, &result, &error);
    if (!ok) fprintf(stderr, "unexpected SQL error: %s: %s\n", sql, error.message);
    adb_result_free(&result);
    check(ok, "query succeeds with injection disabled");
}

typedef enum { PRE_NONE, PRE_READ_TRANSACTION, PRE_DIRTY_TRANSACTION } Preparation;
typedef struct { ADBDatabase *database; ADBSession *session, *observer; } Fixture;

static Fixture fixture_open(Preparation preparation)
{
    ADBError error;
    Fixture fixture = {0};
    fixture.database = adb_open(":memory:", &error);
    check(fixture.database != NULL, "open in-memory fixture");
    fixture.session = adb_session_create(fixture.database, &error);
    fixture.observer = adb_session_create(fixture.database, &error);
    check(fixture.session && fixture.observer, "create independent sessions");
    execute_ok(fixture.session, "CREATE TABLE accounts(id INTEGER PRIMARY KEY, owner TEXT NOT NULL, balance REAL DEFAULT 0, active BOOL DEFAULT TRUE, note TEXT)");
    execute_ok(fixture.session, "INSERT INTO accounts VALUES (1,'Ada',10.5,TRUE,NULL), (2,'Bea',20.0,TRUE,'two'), (3,'Carlos',30.0,FALSE,'three')");
    execute_ok(fixture.session, "CREATE INDEX balance_lookup ON accounts(balance)");
    execute_ok(fixture.session, "CREATE TABLE events(id INTEGER PRIMARY KEY, text TEXT)");
    execute_ok(fixture.session, "INSERT INTO events VALUES (1,'seed')");
    if (preparation != PRE_NONE) execute_ok(fixture.session, "BEGIN");
    if (preparation == PRE_DIRTY_TRANSACTION) {
        execute_ok(fixture.session, "UPDATE accounts SET note='pending', balance=balance+1 WHERE id=1");
        execute_ok(fixture.session, "INSERT INTO events VALUES (2,'transaction')");
    }
    return fixture;
}

static void fixture_close(Fixture *fixture)
{
    adb_session_destroy(fixture->session);
    adb_session_destroy(fixture->observer);
    adb_close(fixture->database);
    memset(fixture, 0, sizeof *fixture);
    if (outstanding) fprintf(stderr, "outstanding allocation count: %ld\n", outstanding);
    check(outstanding == 0, "destroying fixture releases every observed allocation");
}

/* Public API fingerprints observe table schemas, data, row counts, explicit
 * index counts and query-plan index selection in both transaction snapshots. */
static const char *snapshot_queries[] = {
    "SHOW TABLES",
    "DESCRIBE accounts",
    "DESCRIBE events",
    "SELECT * FROM accounts ORDER BY id",
    "SELECT * FROM events ORDER BY id",
    "EXPLAIN SELECT * FROM accounts WHERE owner='Ada'",
    "EXPLAIN SELECT * FROM accounts WHERE balance=10.5"
};
#define SNAPSHOT_PARTS (sizeof snapshot_queries / sizeof snapshot_queries[0])
typedef struct { char *parts[SNAPSHOT_PARTS]; } Snapshot;

static Snapshot snapshot(ADBSession *session)
{
    Snapshot result = {{0}};
    for (size_t i = 0; i < SNAPSHOT_PARTS; ++i) {
        ADBError error;
        ADBResult query;
        bool ok = adb_execute(session, snapshot_queries[i], &query, &error);
        if (!ok) fprintf(stderr, "snapshot query failed: %s: %s\n", snapshot_queries[i], error.message);
        check(ok, "snapshot query succeeds");
        result.parts[i] = adb_result_json(&query);
        adb_result_free(&query);
        check(result.parts[i] != NULL, "serialize snapshot query");
    }
    return result;
}

static void snapshot_free(Snapshot *snapshot)
{
    for (size_t i = 0; i < SNAPSHOT_PARTS; ++i) free(snapshot->parts[i]);
    memset(snapshot, 0, sizeof *snapshot);
}

static void snapshot_compare(const Snapshot *before, const Snapshot *after)
{
    for (size_t i = 0; i < SNAPSHOT_PARTS; ++i) {
        if (strcmp(before->parts[i], after->parts[i]) != 0)
            fprintf(stderr, "changed fingerprint %s\nbefore: %s\nafter:  %s\n",
                    snapshot_queries[i], before->parts[i], after->parts[i]);
        check(strcmp(before->parts[i], after->parts[i]) == 0, "failed operation preserves observable state");
    }
}

typedef struct {
    const char *name;
    const char *sql;
    Preparation preparation;
    bool transaction_after_success;
} Scenario;

static void scenario_check(const Scenario *scenario, size_t *injections)
{
    current_case = scenario->name;
    current_failure = SIZE_MAX;
    Fixture fixture = fixture_open(scenario->preparation);
    ADBError error;
    ADBResult result;
    inject(SIZE_MAX);
    bool ok = adb_execute(fixture.session, scenario->sql, &result, &error);
    size_t allocations = stop_injection();
    if (!ok) fprintf(stderr, "baseline SQL failed: %s: %s\n", scenario->sql, error.message);
    check(ok, "baseline operation succeeds");
    check(allocations > 0 && allocations < 10000, "bounded allocation injection range");
    check(adb_session_in_transaction(fixture.session) == scenario->transaction_after_success,
          "successful operation has expected transaction status");
    adb_result_free(&result);
    fixture_close(&fixture);

    for (size_t failure = 0; failure < allocations; ++failure) {
        current_failure = failure;
        fixture = fixture_open(scenario->preparation);
        bool transaction_before = adb_session_in_transaction(fixture.session);
        Snapshot self_before = snapshot(fixture.session);
        Snapshot observer_before = snapshot(fixture.observer);
        inject(failure);
        ok = adb_execute(fixture.session, scenario->sql, &result, &error);
        (void)stop_injection();
        if (ok) fprintf(stderr, "operation ignored allocation failure: %s\n", scenario->sql);
        check(denied, "selected allocation was reached");
        check(!ok, "failed mandatory allocation fails the operation");
        check(error.code != ADB_OK, "failure reports a public error code");
        adb_result_free(&result);
        check(adb_session_in_transaction(fixture.session) == transaction_before,
              "failed operation preserves transaction status");
        Snapshot self_after = snapshot(fixture.session);
        Snapshot observer_after = snapshot(fixture.observer);
        snapshot_compare(&self_before, &self_after);
        snapshot_compare(&observer_before, &observer_after);
        snapshot_free(&self_before);
        snapshot_free(&observer_before);
        snapshot_free(&self_after);
        snapshot_free(&observer_after);
        execute_ok(fixture.session, scenario->sql);
        check(adb_session_in_transaction(fixture.session) == scenario->transaction_after_success,
              "operation can be retried after allocation failure");
        fixture_close(&fixture);
        ++*injections;
    }
}

static void lifecycle_checks(size_t *injections)
{
    current_case = "open";
    ADBError error;
    inject(SIZE_MAX);
    ADBDatabase *database = adb_open(":memory:", &error);
    size_t allocations = stop_injection();
    check(database != NULL, "baseline open succeeds");
    adb_close(database);
    check(outstanding == 0, "baseline open closes without leaks");
    for (size_t failure = 0; failure < allocations; ++failure) {
        current_failure = failure;
        inject(failure);
        database = adb_open(":memory:", &error);
        (void)stop_injection();
        check(denied && !database && error.code != ADB_OK, "open propagates allocation failure");
        adb_close(database);
        check(outstanding == 0, "failed open releases partial database");
        ++*injections;
    }
    current_case = "session_create";
    database = adb_open(":memory:", &error);
    check(database != NULL, "database for session creation");
    inject(0);
    ADBSession *session = adb_session_create(database, &error);
    (void)stop_injection();
    check(denied && !session && error.code != ADB_OK, "session creation propagates allocation failure");
    adb_session_destroy(session);
    adb_close(database);
    check(outstanding == 0, "failed session creation releases allocations");
    ++*injections;
}

static void json_checks(size_t *injections)
{
    current_case = "result_json";
    Fixture fixture = fixture_open(PRE_NONE);
    ADBError error;
    ADBResult result;
    check(adb_execute(fixture.session, "SELECT * FROM accounts ORDER BY id", &result, &error), "prepare JSON result");
    inject(SIZE_MAX);
    char *expected = adb_result_json(&result);
    size_t allocations = stop_injection();
    check(expected != NULL && allocations > 0, "baseline JSON succeeds");
    for (size_t failure = 0; failure < allocations; ++failure) {
        current_failure = failure;
        long before = outstanding;
        inject(failure);
        char *json = adb_result_json(&result);
        (void)stop_injection();
        check(denied && json == NULL, "JSON formatter propagates allocation failure");
        free(json);
        check(outstanding == before, "failed JSON formatter releases temporary allocations");
        json = adb_result_json(&result);
        check(json && strcmp(json, expected) == 0, "result remains reusable after formatting failure");
        free(json);
        ++*injections;
    }
    free(expected);
    adb_result_free(&result);
    fixture_close(&fixture);
}

int main(void)
{
    static const Scenario scenarios[] = {
        {"create_table", "CREATE TABLE extra(id INTEGER PRIMARY KEY, label TEXT UNIQUE DEFAULT 'new', enabled BOOL DEFAULT TRUE)", PRE_NONE, false},
        {"drop_table", "DROP TABLE events", PRE_NONE, false},
        {"insert_multirow", "INSERT INTO accounts(id,owner,balance,note) VALUES (4,'Dana',40.5,'four'), (5,'Eva',50.0,'five')", PRE_NONE, false},
        {"update", "UPDATE accounts SET owner=UPPER(owner), balance=balance+2.5, note='updated' WHERE id>=2", PRE_NONE, false},
        {"delete", "DELETE FROM accounts WHERE balance>=20", PRE_NONE, false},
        {"create_index", "CREATE UNIQUE INDEX owner_lookup ON accounts(owner)", PRE_NONE, false},
        {"drop_index", "DROP INDEX balance_lookup", PRE_NONE, false},
        {"select_order", "SELECT id, LOWER(owner) AS owner, balance+1 AS total, note FROM accounts WHERE active OR note IS NOT NULL ORDER BY owner DESC LIMIT 2 OFFSET 1", PRE_NONE, false},
        {"select_aggregates", "SELECT COUNT(*), COUNT(note), SUM(balance), AVG(balance), MIN(owner), MAX(note) FROM accounts WHERE id>0", PRE_NONE, false},
        {"select_index", "SELECT * FROM accounts WHERE balance=10.5", PRE_NONE, false},
        {"describe", "DESCRIBE accounts", PRE_NONE, false},
        {"show_tables", "SHOW TABLES", PRE_NONE, false},
        {"explain", "EXPLAIN SELECT * FROM accounts WHERE balance=10.5", PRE_NONE, false},
        {"begin", "BEGIN", PRE_NONE, true},
        {"commit_readonly", "COMMIT", PRE_READ_TRANSACTION, false},
        {"commit_dirty", "COMMIT", PRE_DIRTY_TRANSACTION, false},
        {"rollback_dirty", "ROLLBACK", PRE_DIRTY_TRANSACTION, false},
        {"checkpoint", "CHECKPOINT", PRE_NONE, false},
        {"transaction_create_table", "CREATE TABLE pending(id INT PRIMARY KEY, note TEXT DEFAULT 'pending')", PRE_DIRTY_TRANSACTION, true},
        {"transaction_insert", "INSERT INTO accounts(id,owner) VALUES (4,'Dana'), (5,'Eva')", PRE_DIRTY_TRANSACTION, true},
        {"transaction_update", "UPDATE accounts SET owner=LOWER(owner), balance=balance+1 WHERE id>0", PRE_DIRTY_TRANSACTION, true},
        {"transaction_delete", "DELETE FROM accounts WHERE id>=2", PRE_DIRTY_TRANSACTION, true},
        {"transaction_select", "SELECT * FROM accounts WHERE id>0 ORDER BY owner DESC", PRE_DIRTY_TRANSACTION, true}
    };
    size_t injections = 0;
    lifecycle_checks(&injections);
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i)
        scenario_check(&scenarios[i], &injections);
    json_checks(&injections);
    printf("runtime allocations: %zu checks, %zu failure injections across %zu SQL scenarios\n",
           checks, injections, sizeof scenarios / sizeof scenarios[0]);
    return EXIT_SUCCESS;
}
