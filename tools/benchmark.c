#include "internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool run(ADBSession *session, const char *sql) {
    ADBResult result = {0};
    ADBError error = {0};
    bool ok = adb_execute(session, sql, &result, &error);
    if (!ok) fprintf(stderr, "%s\n", error.message);
    adb_result_free(&result);
    return ok;
}

typedef struct { ADBDatabase *database; size_t rows, queries; bool ok; } Work;
static void *lookups(void *argument) {
    Work *work = argument;
    ADBError error = {0};
    ADBSession *session = adb_session_create(work->database, &error);
    work->ok = session != NULL;
    for (size_t i = 0; work->ok && i < work->queries; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT payload FROM bench WHERE id = %zu", (i * 7919) % work->rows);
        work->ok = run(session, sql);
    }
    adb_session_destroy(session);
    return NULL;
}

int main(int argc, char **argv) {
    size_t rows = 5000;
    if (argc > 2) { fprintf(stderr, "Uso: %s [filas: 1..100000]\n", argv[0]); return 1; }
    if (argc == 2) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || parsed < 1 || parsed > 100000) return 1;
        rows = (size_t)parsed;
    }
    ADBError error = {0};
    ADBDatabase *database = adb_open(":memory:", &error);
    if (!database) { fprintf(stderr, "%s\n", error.message); return 1; }
    ADBSession *session = adb_session_create(database, &error);
    if (!session) { adb_close(database); return 1; }
    bool ok = run(session, "CREATE TABLE bench (id INTEGER PRIMARY KEY, category INTEGER, payload TEXT)");
    double start = seconds();
    for (size_t first = 0; ok && first < rows; first += 500) {
        ADBBuffer sql = {0};
        adb_buffer_append(&sql, "INSERT INTO bench VALUES ", 25);
        size_t last = first + 500 < rows ? first + 500 : rows;
        for (size_t i = first; i < last; i++) {
            if (i != first) adb_buffer_append(&sql, ",", 1);
            adb_buffer_printf(&sql, "(%zu,%zu,'registro-%zu')", i, i % 100, i);
        }
        ok = !sql.failed && run(session, sql.data);
        adb_buffer_free(&sql);
    }
    double insert_time = seconds() - start;
    if (ok) printf("Carga en memoria: %zu filas, %.3f s (%.0f filas/s)\n", rows, insert_time, (double)rows / insert_time);
    const size_t queries = 300;
    start = seconds();
    for (size_t i = 0; ok && i < queries; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT payload FROM bench WHERE id = %zu", (i * 7919) % rows);
        ok = run(session, sql);
    }
    double index_time = seconds() - start;
    if (ok) printf("Indice PRIMARY KEY: %zu consultas, %.3f ms/consulta\n", queries, index_time * 1000.0 / (double)queries);
    start = seconds();
    for (size_t i = 0; ok && i < queries; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM bench WHERE category = %zu", i % 100);
        ok = run(session, sql);
    }
    double scan_time = seconds() - start;
    if (ok) printf("Escaneo completo: %zu consultas, %.3f ms/consulta\n", queries, scan_time * 1000.0 / (double)queries);
    if (ok) {
        pthread_t threads[4];
        Work work[4];
        size_t started = 0;
        start = seconds();
        for (size_t i = 0; i < 4; i++) {
            work[i] = (Work){database, rows, queries, false};
            if (pthread_create(&threads[i], NULL, lookups, &work[i]) != 0) { ok = false; break; }
            started++;
        }
        for (size_t i = 0; i < started; i++) {
            pthread_join(threads[i], NULL);
            if (!work[i].ok) ok = false;
        }
        double concurrent_time = seconds() - start;
        if (ok) printf("4 sesiones: %zu consultas, %.0f consultas/s\n", queries * 4, (double)(queries * 4) / concurrent_time);
    }
    puts("Medicion local, en memoria; incluye parsing y resultados. Las lecturas globales usan un mutex.");
    adb_session_destroy(session);
    adb_close(database);
    return ok ? 0 : 1;
}
