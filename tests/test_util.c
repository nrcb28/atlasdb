#include "internal.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "util: line %d: %s\n", __LINE__, #x); exit(1); } checks++; } while (0)

int main(void) {
    size_t checks = 0;
    ADBError error = {0};
    CHECK(adb_utf8_valid("ASCII"));
    CHECK(adb_utf8_valid("Español 日本語 🌍"));
    CHECK(adb_utf8_valid("\xf4\x8f\xbf\xbf"));
    CHECK(!adb_utf8_valid("\xc0\x80"));
    CHECK(!adb_utf8_valid("\xe0\x80\x80"));
    CHECK(!adb_utf8_valid("\xed\xa0\x80"));
    CHECK(!adb_utf8_valid("\xf4\x90\x80\x80"));
    CHECK(!adb_utf8_valid("\xf0\x9f"));
    CHECK(!adb_utf8_valid("\x80"));

    ADBValue integer = {.type = ADB_INTEGER, .as.integer = INT64_MAX};
    ADBValue real = {.type = ADB_REAL, .as.real = NAN};
    CHECK(adb_value_compare(&integer, &real) < 0);
    CHECK(adb_value_compare(&real, &integer) > 0);
    CHECK(adb_value_compare(&real, &real) == 0);
    real.as.real = INFINITY;
    CHECK(adb_value_compare(&integer, &real) < 0);
    real.as.real = -INFINITY;
    CHECK(adb_value_compare(&integer, &real) > 0);
    real.as.real = 9223372036854775808.0;
    CHECK(!adb_value_coerce(&real, ADB_INTEGER, &error));
    real.as.real = -9223372036854775808.0;
    CHECK(adb_value_coerce(&real, ADB_INTEGER, &error));
    CHECK(real.as.integer == INT64_MIN);
    real = (ADBValue){.type = ADB_REAL, .as.real = 42.5};
    CHECK(!adb_value_coerce(&real, ADB_INTEGER, &error));
    integer.as.integer = 2;
    CHECK(!adb_value_coerce(&integer, ADB_BOOLEAN, &error));
    integer.as.integer = 1;
    CHECK(adb_value_coerce(&integer, ADB_BOOLEAN, &error) && integer.as.boolean);

    ADBResult result = {0};
    const char *names[] = {"\"texto\n"};
    CHECK(adb_result_columns(&result, 1, names, &error));
    ADBValue value = {.type = ADB_TEXT, .as.text = "\"\\\n\t\001Español 🌍"};
    CHECK(adb_result_row(&result, &value, &error));
    char *json = adb_result_json(&result);
    CHECK(json != NULL && adb_utf8_valid(json));
    CHECK(strstr(json, "\\u0001Español 🌍") != NULL);
    CHECK(strstr(json, "\\\"\\\\\\n\\t") != NULL);
    free(json);
    adb_result_free(&result);

    char *large = malloc(ADB_MAX_TEXT + 1);
    CHECK(large != NULL);
    memset(large, 'x', ADB_MAX_TEXT);
    large[ADB_MAX_TEXT] = '\0';
    value.as.text = large;
    CHECK(adb_result_columns(&result, 1, names, &error));
    size_t count = 0;
    while (adb_result_row(&result, &value, &error)) { count++; CHECK(count <= 64); }
    CHECK(error.code == ADB_ERR_LIMIT);
    CHECK(result.nrows == count && count >= 60 && count < 64);
    CHECK(result.text_bytes + result.capacity * sizeof(ADBValue) <= ADB_MAX_RESULT_BYTES);
    CHECK(strlen(result.cells[0].as.text) == ADB_MAX_TEXT);
    CHECK(result.cells[count - 1].as.text[0] == 'x');
    adb_result_free(&result);
    free(large);
    printf("util: %zu checks passed (UTF-8, JSON, numeric bounds, 64 MiB result budget)\n", checks);
    return 0;
}
