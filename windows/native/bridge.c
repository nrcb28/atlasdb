#include "internal.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
/* Only opaque pointers and UTF-8 JSON cross the managed boundary.
 * All strings allocated here must be freed by atlas_free, never by a CLR allocator. */
static char *error_json(const ADBError *error) {
    ADBBuffer buffer = {0};
    if (!adb_buffer_printf(&buffer, "{\"ok\":false,\"code\":%d,\"offset\":%zu,\"error\":",
                           (int)error->code, error->offset) ||
        !adb_buffer_json_string(&buffer, error->message) ||
        !adb_buffer_append(&buffer, "}", 2)) { adb_buffer_free(&buffer); return NULL; }
    return buffer.data;
}
EXPORT void *atlas_open(const char *path, char **error) {
    ADBError failure = {0}; *error = NULL;
    ADBDatabase *db = adb_open(path, &failure);
    if (!db) *error = error_json(&failure);
    return db;
}
EXPORT void atlas_close(void *db) { adb_close(db); }
EXPORT void *atlas_session_create(void *db, char **error) {
    ADBError failure = {0}; *error = NULL;
    ADBSession *session = adb_session_create(db, &failure);
    if (!session) *error = error_json(&failure);
    return session;
}
EXPORT void atlas_session_destroy(void *session) { adb_session_destroy(session); }
EXPORT char *atlas_execute(void *session, const char *sql) {
    ADBError failure = {0}; ADBResult result = {0};
    if (!adb_execute(session, sql, &result, &failure)) return error_json(&failure);
    char *payload = adb_result_json(&result);
    adb_result_free(&result);
    if (!payload) return NULL;
    /* The C API returns the result payload; TCP/managed clients also need ok. */
    size_t length = strlen(payload);
    char *json = malloc(length + 11);
    if (json) { memcpy(json, "{\"ok\":true,", 11); memcpy(json + 11, payload + 1, length); }
    free(payload); return json;
}
EXPORT void atlas_free(void *pointer) { free(pointer); }
