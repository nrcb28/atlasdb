#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Link this executable with GNU ld --wrap=write,--wrap=fsync,
 * --wrap=ftruncate,--wrap=rename. Failures are scoped to one API operation;
 * setup, assertions, cleanup, and recovery always use real system calls. */
ssize_t __real_write(int fd, const void *bytes, size_t length);
int __real_fsync(int fd);
int __real_ftruncate(int fd, off_t length);
int __real_rename(const char *source, const char *destination);

typedef struct {
    bool enabled;
    uint64_t write_fail, sync_fail, truncate_fail, rename_fail;
    uint64_t write_interrupt, sync_interrupt;
    unsigned short_write_call;
    unsigned writes, syncs, truncates, renames, injected;
} Faults;
static Faults faults;
static unsigned assertions;
static const char *scenario;
static ADBError error;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "%s:%d [%s]: %s (%s)\n", __FILE__, __LINE__, scenario, #condition, error.message); \
    exit(1); } } while (0)

static uint64_t call_bit(unsigned call) {
    return call > 0 && call <= 64 ? UINT64_C(1) << (call - 1u) : 0;
}
static bool fail_call(uint64_t mask, unsigned call, int code) {
    if (!(mask & call_bit(call))) return false;
    ++faults.injected; errno = code; return true;
}
ssize_t __wrap_write(int fd, const void *bytes, size_t length) {
    if (faults.enabled) {
        unsigned call = ++faults.writes;
        if (fail_call(faults.write_interrupt, call, EINTR) || fail_call(faults.write_fail, call, EIO)) return -1;
        if (faults.short_write_call == call && length > 1) {
            ++faults.injected;
            faults.write_fail |= call_bit(call + 1u);
            return __real_write(fd, bytes, length / 2u);
        }
    }
    return __real_write(fd, bytes, length);
}
int __wrap_fsync(int fd) {
    if (faults.enabled) {
        unsigned call = ++faults.syncs;
        if (fail_call(faults.sync_interrupt, call, EINTR) || fail_call(faults.sync_fail, call, EIO)) return -1;
    }
    return __real_fsync(fd);
}
int __wrap_ftruncate(int fd, off_t length) {
    if (faults.enabled && fail_call(faults.truncate_fail, ++faults.truncates, EIO)) return -1;
    return __real_ftruncate(fd, length);
}
int __wrap_rename(const char *source, const char *destination) {
    if (faults.enabled && fail_call(faults.rename_fail, ++faults.renames, EIO)) return -1;
    return __real_rename(source, destination);
}
static void arm(Faults plan) { faults = plan; faults.enabled = true; }
static unsigned disarm(void) { faults.enabled = false; return faults.injected; }

static bool execute(ADBSession *session, const char *sql) {
    ADBResult result = {0};
    bool ok = adb_execute(session, sql, &result, &error);
    adb_result_free(&result);
    return ok;
}
static int64_t row_count(ADBSession *session) {
    ADBResult result = {0};
    CHECK(adb_execute(session, "SELECT COUNT(*) FROM events", &result, &error));
    CHECK(result.nrows == 1 && result.ncolumns == 1 && result.cells[0].type == ADB_INTEGER);
    int64_t count = result.cells[0].as.integer; adb_result_free(&result); return count;
}
static void path_name(char *out, size_t size, const char *directory, const char *file) {
    int n = snprintf(out, size, "%s/%s", directory, file); CHECK(n > 0 && (size_t)n < size);
}
static off_t wal_size(const char *directory) {
    char path[512]; struct stat info;
    path_name(path, sizeof(path), directory, "atlas.wal"); CHECK(stat(path, &info) == 0); return info.st_size;
}
static ADBDatabase *open_database(const char *directory, ADBSession **session) {
    ADBDatabase *database = adb_open(directory, &error); CHECK(database != NULL);
    *session = adb_session_create(database, &error); CHECK(*session != NULL); return database;
}
static ADBDatabase *setup(char *directory, ADBSession **session) {
    CHECK(mkdtemp(directory) != NULL);
    ADBDatabase *database = open_database(directory, session);
    CHECK(execute(*session, "CREATE TABLE events (id INTEGER PRIMARY KEY, label TEXT NOT NULL)"));
    CHECK(execute(*session, "INSERT INTO events VALUES (1, 'acknowledged before fault')"));
    return database;
}
static void close_database(ADBDatabase *database, ADBSession *session) {
    adb_session_destroy(session); adb_close(database);
}
static void cleanup(const char *directory) {
    const char *files[] = {"atlas.lock", "atlas.wal", "atlas.snapshot", "atlas.snapshot.tmp"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        char path[512]; path_name(path, sizeof(path), directory, files[i]);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    CHECK(rmdir(directory) == 0);
}

static void clean_append_failure(const char *name, Faults plan) {
    scenario = name;
    char directory[] = "/tmp/atlas-write-fault-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    off_t baseline = wal_size(directory); uint64_t version = database->state->version;
    arm(plan);
    bool ok = execute(session, "INSERT INTO events VALUES (2, 'must roll back')");
    unsigned injected = disarm();
    CHECK(!ok && error.code == ADB_ERR_IO && injected > 0);
    CHECK(database->storage.wal_fd >= 0 && database->state->version == version);
    CHECK(wal_size(directory) == baseline && row_count(session) == 1);
    CHECK(execute(session, "INSERT INTO events VALUES (2, 'acknowledged retry')"));
    CHECK(row_count(session) == 2);
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 2);
    close_database(database, session); cleanup(directory);
}

static void retry_transaction_commit(void) {
    scenario = "explicit COMMIT retry after fsync failure";
    char directory[] = "/tmp/atlas-transaction-fault-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    ADBSession *observer = adb_session_create(database, &error); CHECK(observer != NULL);
    CHECK(execute(session, "BEGIN"));
    CHECK(execute(session, "INSERT INTO events VALUES (2, 'transaction retry')"));
    off_t baseline = wal_size(directory);
    arm((Faults){.sync_fail=call_bit(1)});
    bool ok = execute(session, "COMMIT");
    CHECK(disarm() == 1);
    CHECK(!ok && error.code == ADB_ERR_IO && adb_session_in_transaction(session));
    CHECK(row_count(session) == 2 && row_count(observer) == 1 && wal_size(directory) == baseline);
    CHECK(execute(session, "COMMIT"));
    CHECK(!adb_session_in_transaction(session) && row_count(observer) == 2);
    adb_session_destroy(observer); close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 2);
    close_database(database, session); cleanup(directory);
}

static void poisoned_append(const char *name, Faults plan, int64_t recovered_rows) {
    scenario = name;
    char directory[] = "/tmp/atlas-poison-fault-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    arm(plan);
    bool ok = execute(session, "INSERT INTO events VALUES (2, 'uncertain IO result')");
    unsigned injected = disarm();
    CHECK(!ok && error.code == ADB_ERR_IO && injected >= 2);
    CHECK(strstr(error.message, "uncertain") != NULL);
    CHECK(database->storage.wal_fd == -1 && row_count(session) == 1);
    /* The poisoned backend must reject subsequent writes and checkpoints; no
     * in-memory acknowledgement may outrun the uncertain disk state. */
    CHECK(!execute(session, "INSERT INTO events VALUES (3, 'must be refused')") && error.code == ADB_ERR_IO);
    CHECK(row_count(session) == 1);
    CHECK(!adb_checkpoint(database, &error) && error.code == ADB_ERR_IO);
    close_database(database, session);
    database = open_database(directory, &session);
    CHECK(row_count(session) == recovered_rows);
    /* All rows acknowledged before the failing call are preserved even when
     * a complete uncertain record is recovered after a failed truncate. */
    ADBResult result = {0};
    CHECK(adb_execute(session, "SELECT label FROM events WHERE id = 1", &result, &error));
    CHECK(result.nrows == 1 && strcmp(result.cells[0].as.text, "acknowledged before fault") == 0);
    adb_result_free(&result);
    CHECK(execute(session, "INSERT INTO events VALUES (3, 'new backend usable')"));
    close_database(database, session); cleanup(directory);
}

static void checkpoint_failure(const char *name, Faults plan, bool previous_snapshot) {
    scenario = name;
    char directory[] = "/tmp/atlas-checkpoint-fault-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    if (previous_snapshot) CHECK(adb_checkpoint(database, &error));
    CHECK(execute(session, "INSERT INTO events VALUES (2, 'acknowledged WAL row')"));
    arm(plan);
    bool ok = adb_checkpoint(database, &error);
    unsigned injected = disarm();
    CHECK(!ok && error.code == ADB_ERR_IO && injected > 0);
    CHECK(database->storage.wal_fd >= 0 && row_count(session) == 2);
    char temporary[512]; path_name(temporary, sizeof(temporary), directory, "atlas.snapshot.tmp");
    CHECK(access(temporary, F_OK) < 0 && errno == ENOENT);
    /* Reopen before any further commit: a new full-state commit could mask a
     * checkpoint data-loss bug by serializing the still-live in-memory rows. */
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 2);
    CHECK(execute(session, "INSERT INTO events VALUES (3, 'acknowledged after checkpoint failure')"));
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 3);
    CHECK(adb_checkpoint(database, &error) && wal_size(directory) == 0);
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 3);
    close_database(database, session); cleanup(directory);
}

static void checkpoint_continue_after_failed_sync(void) {
    scenario = "continue immediately after WAL truncate and failed fsync";
    char directory[] = "/tmp/atlas-checkpoint-offset-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    CHECK(execute(session, "INSERT INTO events VALUES (2, 'acknowledged before checkpoint')"));
    arm((Faults){.sync_fail=call_bit(3)});
    bool ok = adb_checkpoint(database, &error);
    CHECK(disarm() == 1 && !ok && error.code == ADB_ERR_IO);
    CHECK(wal_size(directory) == 0);
    /* ftruncate leaves the old file offset intact. The next append must seek
     * to the new end instead of creating a hole before its first frame. */
    CHECK(execute(session, "INSERT INTO events VALUES (3, 'acknowledged without reopen')"));
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 3);
    close_database(database, session); cleanup(directory);
}

static void interrupted_calls(void) {
    scenario = "EINTR is retried for write and fsync";
    char directory[] = "/tmp/atlas-interrupt-fault-XXXXXX";
    ADBSession *session = NULL; ADBDatabase *database = setup(directory, &session);
    arm((Faults){.write_interrupt=call_bit(1), .sync_interrupt=call_bit(1)});
    bool ok = execute(session, "INSERT INTO events VALUES (2, 'interrupted syscalls')");
    CHECK(disarm() == 2 && ok);
    close_database(database, session);
    database = open_database(directory, &session); CHECK(row_count(session) == 2);
    close_database(database, session); cleanup(directory);
}

int main(void) {
    clean_append_failure("append header write fails", (Faults){.write_fail=call_bit(1)});
    clean_append_failure("append payload write fails", (Faults){.write_fail=call_bit(2)});
    clean_append_failure("append partial payload then write fails", (Faults){.short_write_call=2});
    clean_append_failure("append fsync fails", (Faults){.sync_fail=call_bit(1)});
    retry_transaction_commit();
    poisoned_append("partial append and rollback truncate fail", (Faults){.write_fail=call_bit(2), .truncate_fail=call_bit(1)}, 1);
    poisoned_append("complete append fsync and rollback truncate fail", (Faults){.sync_fail=call_bit(1), .truncate_fail=call_bit(1)}, 2);
    poisoned_append("commit fsync and rollback fsync fail", (Faults){.sync_fail=call_bit(1)|call_bit(2)}, 1);
    for (unsigned snapshot = 0; snapshot < 2; ++snapshot) {
        checkpoint_failure("checkpoint header write fails", (Faults){.write_fail=call_bit(1)}, snapshot != 0);
        checkpoint_failure("checkpoint payload write fails", (Faults){.write_fail=call_bit(2)}, snapshot != 0);
        checkpoint_failure("checkpoint partial payload write fails", (Faults){.short_write_call=2}, snapshot != 0);
        checkpoint_failure("checkpoint temporary file fsync fails", (Faults){.sync_fail=call_bit(1)}, snapshot != 0);
        checkpoint_failure("checkpoint rename fails", (Faults){.rename_fail=call_bit(1)}, snapshot != 0);
        checkpoint_failure("checkpoint directory fsync fails", (Faults){.sync_fail=call_bit(2)}, snapshot != 0);
        checkpoint_failure("checkpoint WAL truncate fails", (Faults){.truncate_fail=call_bit(1)}, snapshot != 0);
        checkpoint_failure("checkpoint WAL fsync fails", (Faults){.sync_fail=call_bit(3)}, snapshot != 0);
    }
    checkpoint_continue_after_failed_sync();
    interrupted_calls();
    printf("storage faults: %u assertions passed across 26 fault scenarios\n", assertions);
    return 0;
}
