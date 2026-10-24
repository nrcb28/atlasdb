#ifndef ATLASDB_H
#define ATLASDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADB_VERSION "1.0.0"
#define ADB_MAX_SQL (1024u * 1024u)
#define ADB_MAX_COLUMNS 128u
#define ADB_MAX_TEXT (1024u * 1024u)
#define ADB_MAX_RESULT_BYTES (64u * 1024u * 1024u)

typedef enum { ADB_NULL, ADB_INTEGER, ADB_REAL, ADB_TEXT, ADB_BOOLEAN } ADBType;
typedef struct {
    ADBType type;
    union { int64_t integer; double real; char *text; bool boolean; } as;
} ADBValue;
typedef enum {
    ADB_OK = 0, ADB_ERR_PARSE, ADB_ERR_SCHEMA, ADB_ERR_TYPE,
    ADB_ERR_CONSTRAINT, ADB_ERR_IO, ADB_ERR_MEMORY, ADB_ERR_TRANSACTION,
    ADB_ERR_CONFLICT, ADB_ERR_LIMIT, ADB_ERR_CORRUPT
} ADBCode;
typedef struct { ADBCode code; size_t offset; char message[512]; } ADBError;
typedef struct {
    char **columns;
    size_t ncolumns;
    ADBValue *cells;
    size_t nrows;
    size_t capacity;
    size_t text_bytes;
    uint64_t affected;
    char *message;
} ADBResult;
typedef struct ADBDatabase ADBDatabase;
typedef struct ADBSession ADBSession;

/* A database owns sessions; destroy sessions before closing it. A session is
 * confined to one thread. Distinct sessions may execute concurrently. */
ADBDatabase *adb_open(const char *directory, ADBError *error);
void adb_close(ADBDatabase *database);
ADBSession *adb_session_create(ADBDatabase *database, ADBError *error);
void adb_session_destroy(ADBSession *session);
/* Executes one SQL statement; an optional final semicolon is accepted.
 * Free the previous result before reusing it. SQL and TEXT use valid UTF-8.
 * The host process must keep the C numeric locale while calling this API. */
bool adb_execute(ADBSession *session, const char *sql, ADBResult *result, ADBError *error);
bool adb_checkpoint(ADBDatabase *database, ADBError *error);
bool adb_session_in_transaction(const ADBSession *session);
void adb_result_free(ADBResult *result);
/* Allocated string; caller frees it. JSON output includes column and row arrays. */
char *adb_result_json(const ADBResult *result);
const char *adb_type_name(ADBType type);
void adb_value_free(ADBValue *value);

#ifdef __cplusplus
}
#endif
#endif
