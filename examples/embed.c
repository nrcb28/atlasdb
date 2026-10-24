/* cc -std=c17 -Iinclude examples/embed.c build/libatlasdb.a -pthread -lm -o build/embed */
#include "atlasdb.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    ADBError error = {0};
    ADBDatabase *database = adb_open(":memory:", &error);
    if (!database) { fprintf(stderr, "%s\n", error.message); return 1; }
    ADBSession *session = adb_session_create(database, &error);
    if (!session) { fprintf(stderr, "%s\n", error.message); adb_close(database); return 1; }
    const char *queries[] = {
        "CREATE TABLE sensors (id INTEGER PRIMARY KEY, name TEXT NOT NULL, temperature REAL)",
        "BEGIN",
        "INSERT INTO sensors VALUES (1, 'Laboratorio', 21.5), (2, 'Almacen', 18.2)",
        "UPDATE sensors SET temperature = temperature + 0.5 WHERE id = 1",
        "COMMIT",
        "SELECT name, temperature FROM sensors ORDER BY temperature DESC"
    };
    int status = 0;
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        ADBResult result = {0};
        if (!adb_execute(session, queries[i], &result, &error)) {
            fprintf(stderr, "Error %d: %s\n", (int)error.code, error.message);
            status = 1;
            break;
        }
        char *json = adb_result_json(&result);
        if (!json) { adb_result_free(&result); status = 1; break; }
        puts(json);
        free(json);
        adb_result_free(&result);
    }
    adb_session_destroy(session); /* An outstanding transaction is rolled back. */
    adb_close(database);
    return status;
}
