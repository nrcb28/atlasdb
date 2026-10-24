/* Optional GNU ld allocation-failure test. Link with:
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * Each run fails exactly one program allocation and checks complete cleanup.
 * Wrappers do not observe allocations private to shared system libraries. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

static long outstanding;
static long remaining = -1;

static bool deny_allocation(void)
{
    if (remaining < 0) return false;
    return remaining-- == 0;
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
    void *replacement = __real_realloc(pointer, size);
    if (replacement && !pointer) ++outstanding;
    return replacement;
}

void __wrap_free(void *pointer)
{
    if (pointer) --outstanding;
    __real_free(pointer);
}

int main(void)
{
    static const char *inputs[] = {
        "CREATE TABLE t(id INT PRIMARY KEY, a TEXT DEFAULT 'can''t', b REAL DEFAULT -1.5, c BOOL DEFAULT FALSE)",
        "INSERT INTO t(id,a) VALUES (1,'alpha'), (2,'beta'), (3,'gamma')",
        "SELECT a+3*2 AS total, SUM(a), LOWER(b) FROM t WHERE NOT a=1 OR b LIKE 'a_%' ORDER BY a DESC LIMIT 2",
        "UPDATE t SET a=LOWER(a), b=b+1 WHERE a IS NOT NULL",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx ON t(a)",
        "EXPLAIN SELECT * FROM t WHERE a=3"
    };
    size_t checks = 0;
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        bool completed = false;
        for (long failure = 0; failure < 160; ++failure) {
            ADBError error;
            remaining = failure;
            ADBStatement *statement = adb_parse(inputs[i], &error);
            remaining = -1;
            if (statement) completed = true;
            else if (error.code == ADB_OK) {
                fprintf(stderr, "missing error for SQL %zu, allocation %ld\n", i, failure);
                return EXIT_FAILURE;
            }
            adb_statement_free(statement);
            if (outstanding != 0) {
                fprintf(stderr, "%ld outstanding allocations for SQL %zu, allocation %ld\n", outstanding, i, failure);
                return EXIT_FAILURE;
            }
            ++checks;
        }
        if (!completed) {
            fprintf(stderr, "SQL %zu never completed; increase injection range\n", i);
            return EXIT_FAILURE;
        }
    }
    printf("parser allocations: %zu failure injections without outstanding allocations\n", checks);
    return EXIT_SUCCESS;
}
