#include "internal.h"
#include <stdlib.h>
#include <string.h>

ADBDatabase *adb_open(const char *directory, ADBError *error) {
    if (error) memset(error, 0, sizeof(*error));
    ADBDatabase *database = adb_alloc(1, sizeof(*database), error);
    if (!database) return NULL;
    database->storage.lock_fd = database->storage.wal_fd = -1;
    if (pthread_mutex_init(&database->mutex, NULL) != 0) {
        adb_error(error, ADB_ERR_MEMORY, "Could not initialize database mutex");
        free(database);
        return NULL;
    }
    if (!adb_storage_open(&database->storage, directory ? directory : ":memory:", &database->state, error)) {
        adb_storage_close(&database->storage);
        pthread_mutex_destroy(&database->mutex);
        free(database);
        return NULL;
    }
    return database;
}

void adb_close(ADBDatabase *database) {
    if (!database) return;
    adb_state_free(database->state);
    adb_storage_close(&database->storage);
    pthread_mutex_destroy(&database->mutex);
    free(database);
}

ADBSession *adb_session_create(ADBDatabase *database, ADBError *error) {
    if (!database) { adb_error(error, ADB_ERR_SCHEMA, "Database is required"); return NULL; }
    ADBSession *session = adb_alloc(1, sizeof(*session), error);
    if (session) session->database = database;
    return session;
}

void adb_session_destroy(ADBSession *session) {
    if (!session) return;
    adb_state_free(session->transaction);
    free(session);
}

bool adb_session_in_transaction(const ADBSession *session) {
    return session && session->transaction;
}

bool adb_checkpoint(ADBDatabase *database, ADBError *error) {
    if (error) memset(error, 0, sizeof(*error));
    if (!database) return adb_error(error, ADB_ERR_SCHEMA, "Database is required");
    pthread_mutex_lock(&database->mutex);
    bool ok = adb_storage_checkpoint(&database->storage, database->state, error);
    pthread_mutex_unlock(&database->mutex);
    return ok;
}

static bool transaction_begin(ADBSession *session, ADBResult *result, ADBError *error) {
    if (session->transaction) return adb_error(error, ADB_ERR_TRANSACTION, "A transaction is already active");
    if (!adb_result_message(result, "Transaction started")) return false;
    pthread_mutex_lock(&session->database->mutex);
    session->transaction = adb_state_clone(session->database->state, error);
    if (session->transaction) session->base_version = session->database->state->version;
    pthread_mutex_unlock(&session->database->mutex);
    if (!session->transaction) return false;
    session->dirty = false;
    return true;
}

static bool transaction_commit(ADBSession *session, ADBResult *result, ADBError *error) {
    if (!session->transaction) return adb_error(error, ADB_ERR_TRANSACTION, "No active transaction");
    if (!adb_result_message(result, session->dirty ? "Transaction committed" : "Read-only transaction committed")) return false;
    if (!session->dirty) {
        adb_state_free(session->transaction);
        session->transaction = NULL;
        return true;
    }
    ADBDatabase *database = session->database;
    pthread_mutex_lock(&database->mutex);
    if (database->state->version != session->base_version) {
        pthread_mutex_unlock(&database->mutex);
        return adb_error(error, ADB_ERR_CONFLICT, "Transaction conflict: another session committed; ROLLBACK and retry");
    }
    if (database->state->version == UINT64_MAX) {
        pthread_mutex_unlock(&database->mutex);
        return adb_error(error, ADB_ERR_LIMIT, "Database version limit reached");
    }
    session->transaction->version = database->state->version + 1;
    bool ok = adb_storage_commit(&database->storage, session->transaction, error);
    if (ok) {
        ADBState *old = database->state;
        database->state = session->transaction;
        session->transaction = NULL;
        session->dirty = false;
        adb_state_free(old);
    }
    pthread_mutex_unlock(&database->mutex);
    return ok;
}

static bool execute_regular(ADBSession *session, const ADBStatement *statement, ADBResult *result, ADBError *error) {
    bool writes = adb_statement_writes(statement);
    bool changed = false;
    ADBDatabase *database = session->database;
    if (session->transaction) {
        if (!writes) return adb_engine_execute(session->transaction, statement, result, &changed, error);
        ADBState *working = adb_state_clone(session->transaction, error);
        if (!working) return false;
        bool ok = adb_engine_execute(working, statement, result, &changed, error);
        if (ok && changed) {
            adb_state_free(session->transaction);
            session->transaction = working;
            session->dirty = true;
        } else adb_state_free(working);
        return ok;
    }
    pthread_mutex_lock(&database->mutex);
    if (!writes) {
        bool ok = adb_engine_execute(database->state, statement, result, &changed, error);
        pthread_mutex_unlock(&database->mutex);
        return ok;
    }
    ADBState *working = adb_state_clone(database->state, error);
    if (!working) { pthread_mutex_unlock(&database->mutex); return false; }
    bool ok = adb_engine_execute(working, statement, result, &changed, error);
    if (ok && changed) {
        if (database->state->version == UINT64_MAX) ok = adb_error(error, ADB_ERR_LIMIT, "Database version limit reached");
        else {
            working->version = database->state->version + 1;
            ok = adb_storage_commit(&database->storage, working, error);
            if (ok) {
                ADBState *old = database->state;
                database->state = working;
                working = old;
            }
        }
    }
    adb_state_free(working);
    pthread_mutex_unlock(&database->mutex);
    return ok;
}

bool adb_execute(ADBSession *session, const char *sql, ADBResult *result, ADBError *error) {
    if (error) memset(error, 0, sizeof(*error));
    if (!result) return adb_error(error, ADB_ERR_SCHEMA, "Result is required");
    memset(result, 0, sizeof(*result));
    if (!session || !sql) return adb_error(error, ADB_ERR_SCHEMA, "Session and SQL are required");
    if (strnlen(sql, ADB_MAX_SQL + 1) > ADB_MAX_SQL) return adb_error(error, ADB_ERR_LIMIT, "SQL exceeds 1 MiB");
    ADBStatement *statement = adb_parse(sql, error);
    if (!statement) return false;
    bool ok;
    switch (statement->kind) {
    case ST_BEGIN: ok = transaction_begin(session, result, error); break;
    case ST_COMMIT: ok = transaction_commit(session, result, error); break;
    case ST_ROLLBACK:
        if (!session->transaction) ok = adb_error(error, ADB_ERR_TRANSACTION, "No active transaction");
        else if (!adb_result_message(result, "Transaction rolled back")) ok = false;
        else {
            adb_state_free(session->transaction);
            session->transaction = NULL;
            session->dirty = false;
            ok = true;
        }
        break;
    case ST_CHECKPOINT:
        if (session->transaction) ok = adb_error(error, ADB_ERR_TRANSACTION, "CHECKPOINT requires a session without an active transaction");
        else ok = adb_result_message(result, "Checkpoint complete") && adb_checkpoint(session->database, error);
        break;
    default: ok = execute_regular(session, statement, result, error); break;
    }
    adb_statement_free(statement);
    if (!ok) {
        adb_result_free(result);
        if (error && error->code == ADB_OK) adb_error(error, ADB_ERR_MEMORY, "Could not allocate query result");
    }
    return ok;
}
