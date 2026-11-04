#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    if (!data || size > ADB_MAX_SQL) return 0;
    char *sql = malloc(size + 1u);
    if (!sql) return 0;
    memcpy(sql, data, size);
    sql[size] = '\0';
    ADBError error = {0};
    ADBStatement *statement = adb_parse(sql, &error);
    adb_statement_free(statement);
    free(sql);
    return 0;
}
