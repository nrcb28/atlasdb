#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned assertions;
static ADBError error;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "%s:%d: assertion failed: %s (%s)\n", __FILE__, __LINE__, #condition, error.message); \
    exit(1); } } while (0)

static char *copy_text(const char *text) {
    char *out = adb_strdup(text, &error); CHECK(out != NULL); return out;
}
static ADBState *fixture(uint64_t version) {
    ADBState *state = adb_state_create(&error); CHECK(state != NULL);
    state->version = version;
    state->tables = adb_alloc(1, sizeof(*state->tables), &error); CHECK(state->tables != NULL);
    state->ntables = 1;
    ADBTable *table = state->tables;
    table->name = copy_text("measurements"); table->ncolumns = 5;
    table->columns = adb_alloc(table->ncolumns, sizeof(*table->columns), &error); CHECK(table->columns != NULL);
    const char *names[] = {"id", "temperature", "label", "enabled", "optional"};
    const ADBType types[] = {ADB_INTEGER, ADB_REAL, ADB_TEXT, ADB_BOOLEAN, ADB_INTEGER};
    for (size_t c = 0; c < table->ncolumns; ++c) {
        table->columns[c].name = copy_text(names[c]); table->columns[c].type = types[c];
    }
    table->columns[0].primary_key = table->columns[0].unique = table->columns[0].not_null = true;
    table->columns[1].has_default = true;
    table->columns[1].default_value = (ADBValue){.type=ADB_REAL, .as.real=1.25};
    table->columns[2].has_default = true;
    table->columns[2].default_value = (ADBValue){.type=ADB_TEXT, .as.text=copy_text("fallback")};
    table->nrows = table->row_capacity = 2; table->next_row_id = 3;
    table->rows = adb_alloc(table->nrows, sizeof(*table->rows), &error); CHECK(table->rows != NULL);
    for (size_t r = 0; r < table->nrows; ++r) {
        table->rows[r].id = r + 1u;
        table->rows[r].values = adb_alloc(table->ncolumns, sizeof(ADBValue), &error); CHECK(table->rows[r].values != NULL);
        ADBValue *v = table->rows[r].values;
        v[0] = (ADBValue){.type=ADB_INTEGER, .as.integer=r ? INT64_MAX : INT64_MIN};
        v[1] = (ADBValue){.type=ADB_REAL, .as.real=r ? 1e100 : -0.25};
        v[2] = (ADBValue){.type=ADB_TEXT, .as.text=copy_text(r ? "" : "a\rb\nc\t\"\\ UTF-8: á")};
        v[3] = (ADBValue){.type=ADB_BOOLEAN, .as.boolean=r == 0};
        v[4] = r ? (ADBValue){.type=ADB_INTEGER, .as.integer=-9} : adb_null();
    }
    table->nindexes = 2;
    table->indexes = adb_alloc(table->nindexes, sizeof(*table->indexes), &error); CHECK(table->indexes != NULL);
    table->indexes[0] = (ADBIndex){.name=copy_text("pk_measurements"), .column=0, .unique=true, .implicit=true};
    table->indexes[1] = (ADBIndex){.name=copy_text("by_label"), .column=2};
    CHECK(adb_table_reindex(table, &error));
    return state;
}
static void file_path(char *buffer, size_t size, const char *directory, const char *name) {
    int n = snprintf(buffer, size, "%s/%s", directory, name); CHECK(n > 0 && (size_t)n < size);
}
static off_t file_size(const char *path) {
    struct stat info; CHECK(stat(path, &info) == 0); return info.st_size;
}
static void append_bytes(const char *path, const void *bytes, size_t length) {
    int fd = open(path, O_WRONLY | O_APPEND); CHECK(fd >= 0);
    CHECK(write(fd, bytes, length) == (ssize_t)length); CHECK(close(fd) == 0);
}
static void cleanup(const char *directory) {
    const char *files[] = {"atlas.lock", "atlas.wal", "atlas.snapshot", "atlas.snapshot.tmp"};
    for (size_t i = 0; i < sizeof(files)/sizeof(*files); ++i) {
        char path[512]; file_path(path, sizeof(path), directory, files[i]);
        CHECK(unlink(path) == 0 || errno == ENOENT);
    }
    CHECK(rmdir(directory) == 0);
}

static void codec_tests(void) {
    CHECK(adb_crc32("123456789", 9) == UINT32_C(0xcbf43926));
    ADBState *state = fixture(42), *decoded;
    unsigned char *bytes = NULL, *again = NULL; size_t length = 0, again_length = 0;
    CHECK(adb_state_encode(state, &bytes, &length, &error));
    decoded = adb_state_decode(bytes, length, &error); CHECK(decoded != NULL);
    CHECK(decoded->version == 42 && decoded->ntables == 1);
    CHECK(decoded->tables[0].rows[0].values[0].as.integer == INT64_MIN);
    CHECK(decoded->tables[0].rows[1].values[0].as.integer == INT64_MAX);
    CHECK(decoded->tables[0].rows[1].values[1].as.real == 1e100);
    CHECK(decoded->tables[0].indexes[0].implicit && decoded->tables[0].indexes[0].unique);
    size_t count = 0; ADBValue key = {.type=ADB_INTEGER, .as.integer=INT64_MIN};
    const uint64_t *ids = adb_tree_find(decoded->tables[0].indexes[0].tree, &key, &count);
    CHECK(ids != NULL && count == 1 && ids[0] == 1);
    CHECK(adb_state_encode(decoded, &again, &again_length, &error));
    CHECK(length == again_length && memcmp(bytes, again, length) == 0);
    free(again); adb_state_free(decoded);
    /* Every proper payload prefix must fail cleanly, including partially
     * constructed text values, schema arrays, index arrays, and row arrays. */
    for (size_t cut = 0; cut < length; ++cut) {
        error = (ADBError){0};
        decoded = adb_state_decode(bytes, cut, &error);
        CHECK(decoded == NULL && error.code == ADB_ERR_CORRUPT);
    }
    /* Exercise field validation at every byte. Some mutations legitimately
     * change a value; those must still decode and release safely. */
    for (size_t offset = 0; offset < length; ++offset) {
        unsigned char saved = bytes[offset]; bytes[offset] ^= 0xffu;
        decoded = adb_state_decode(bytes, length, &error);
        adb_state_free(decoded); bytes[offset] = saved;
    }
    unsigned char saved_count[4]; memcpy(saved_count, bytes + 8, 4);
    memset(bytes + 8, 0xff, 4);
    decoded = adb_state_decode(bytes, length, &error); CHECK(decoded == NULL && error.code == ADB_ERR_CORRUPT);
    memcpy(bytes + 8, saved_count, 4);
    unsigned char saved_name = bytes[16]; bytes[16] = 0xffu;
    decoded = adb_state_decode(bytes, length, &error); CHECK(decoded == NULL && error.code == ADB_ERR_CORRUPT);
    bytes[16] = saved_name;
    state->tables[0].rows[0].values[1].as.real = NAN;
    unsigned char *invalid = NULL; size_t invalid_length = 0;
    CHECK(!adb_state_encode(state, &invalid, &invalid_length, &error) && error.code == ADB_ERR_TYPE);
    CHECK(invalid == NULL && invalid_length == 0);
    state->tables[0].rows[0].values[1].as.real = -0.25;
    char original = state->tables[0].name[0]; state->tables[0].name[0] = (char)0xff;
    CHECK(!adb_state_encode(state, &invalid, &invalid_length, &error) && error.code == ADB_ERR_TYPE);
    state->tables[0].name[0] = original;
    state->tables[0].nindexes = 4097;
    CHECK(!adb_state_encode(state, &invalid, &invalid_length, &error) && error.code == ADB_ERR_LIMIT);
    state->tables[0].nindexes = 2;
    free(bytes); adb_state_free(state);
    /* Index names share a global namespace, including implicit constraints. */
    state = fixture(43);
    ADBState *duplicate = fixture(43);
    ADBTable *tables = realloc(state->tables, 2 * sizeof(*tables)); CHECK(tables != NULL);
    state->tables = tables; state->tables[1] = duplicate->tables[0]; state->ntables = 2;
    duplicate->ntables = 0; adb_state_free(duplicate);
    free(state->tables[1].name); state->tables[1].name = copy_text("other_measurements");
    CHECK(adb_state_encode(state, &bytes, &length, &error));
    decoded = adb_state_decode(bytes, length, &error); CHECK(decoded == NULL && error.code == ADB_ERR_CORRUPT);
    free(bytes); adb_state_free(state);
    state = adb_state_create(&error); CHECK(state != NULL);
    CHECK(adb_state_encode(state, &bytes, &length, &error) && length == 12);
    decoded = adb_state_decode(bytes, length, &error); CHECK(decoded && decoded->ntables == 0);
    free(bytes); adb_state_free(state); adb_state_free(decoded);
}

static void disk_tests(void) {
    char directory[] = "/tmp/atlas-storage-XXXXXX"; CHECK(mkdtemp(directory) != NULL);
    char wal[512], snapshot[512];
    file_path(wal, sizeof(wal), directory, "atlas.wal");
    file_path(snapshot, sizeof(snapshot), directory, "atlas.snapshot");
    ADBStorage storage, second; ADBState *state = NULL, *other = NULL;
    CHECK(adb_storage_open(&storage, directory, &state, &error)); CHECK(state->version == 0);
    CHECK(!adb_storage_open(&second, directory, &other, &error) && error.code == ADB_ERR_IO);
    CHECK(other == NULL);
    pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) {
        ADBStorage locked; ADBState *child_state = NULL; ADBError child_error = {0};
        bool obtained = adb_storage_open(&locked, directory, &child_state, &child_error);
        if (obtained) { adb_storage_close(&locked); adb_state_free(child_state); }
        _exit(obtained ? 1 : 0);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    adb_state_free(state); state = fixture(1);
    CHECK(adb_storage_commit(&storage, state, &error));
    state->version = 2; CHECK(adb_storage_commit(&storage, state, &error));
    off_t complete = file_size(wal); CHECK(complete > 64);
    adb_storage_close(&storage); adb_state_free(state);
    const unsigned char torn_header[] = {'A','T','L','A','S'};
    append_bytes(wal, torn_header, sizeof(torn_header));
    CHECK(adb_storage_open(&storage, directory, &state, &error));
    CHECK(state->version == 2 && file_size(wal) == complete);
    adb_storage_close(&storage); adb_state_free(state);
    /* Append a valid complete frame header followed by a partial payload. */
    unsigned char prefix[42];
    int fd = open(wal, O_RDONLY); CHECK(fd >= 0); CHECK(read(fd, prefix, sizeof(prefix)) == (ssize_t)sizeof(prefix)); CHECK(close(fd) == 0);
    append_bytes(wal, prefix, sizeof(prefix));
    CHECK(adb_storage_open(&storage, directory, &state, &error));
    CHECK(state->version == 2 && file_size(wal) == complete);
    CHECK(adb_storage_checkpoint(&storage, state, &error));
    CHECK(file_size(wal) == 0 && file_size(snapshot) > 32);
    adb_storage_close(&storage); adb_state_free(state);
    CHECK(adb_storage_open(&storage, directory, &state, &error));
    CHECK(state->version == 2 && state->tables[0].nrows == 2);
    state->version = 3; CHECK(adb_storage_commit(&storage, state, &error));
    /* Save WAL, checkpoint, then put the old WAL back to simulate a crash
     * between durable snapshot publication and WAL truncation. */
    off_t old_size = file_size(wal); CHECK(old_size > 0);
    unsigned char *old_wal = malloc((size_t)old_size); CHECK(old_wal != NULL);
    fd = open(wal, O_RDONLY); CHECK(fd >= 0); CHECK(read(fd, old_wal, (size_t)old_size) == old_size); CHECK(close(fd) == 0);
    CHECK(adb_storage_checkpoint(&storage, state, &error));
    adb_storage_close(&storage); adb_state_free(state);
    append_bytes(wal, old_wal, (size_t)old_size); free(old_wal);
    CHECK(adb_storage_open(&storage, directory, &state, &error)); CHECK(state->version == 3);
    state->version = 4; CHECK(adb_storage_commit(&storage, state, &error));
    adb_storage_close(&storage); adb_state_free(state);
    CHECK(adb_storage_open(&storage, directory, &state, &error)); CHECK(state->version == 4);
    adb_storage_close(&storage); adb_state_free(state);
    /* Corruption in a complete WAL payload is rejected rather than silently
     * discarded as a torn record. */
    fd = open(wal, O_RDWR); CHECK(fd >= 0);
    unsigned char byte; CHECK(pread(fd, &byte, 1, 32) == 1);
    unsigned char flipped = byte ^ 0x80u; CHECK(pwrite(fd, &flipped, 1, 32) == 1); CHECK(close(fd) == 0);
    CHECK(!adb_storage_open(&storage, directory, &state, &error) && error.code == ADB_ERR_CORRUPT); CHECK(state == NULL);
    fd = open(wal, O_RDWR); CHECK(fd >= 0); CHECK(pwrite(fd, &byte, 1, 32) == 1); CHECK(close(fd) == 0);
    CHECK(adb_storage_open(&storage, directory, &state, &error)); CHECK(state->version == 4);
    CHECK(adb_storage_checkpoint(&storage, state, &error));
    adb_storage_close(&storage); adb_state_free(state);
    fd = open(snapshot, O_WRONLY | O_APPEND); CHECK(fd >= 0); CHECK(write(fd, "x", 1) == 1); CHECK(close(fd) == 0);
    CHECK(!adb_storage_open(&storage, directory, &state, &error) && error.code == ADB_ERR_CORRUPT); CHECK(state == NULL);
    cleanup(directory);
}

static void memory_tests(void) {
    ADBStorage storage; ADBState *state = NULL;
    CHECK(adb_storage_open(&storage, ":memory:", &state, &error));
    CHECK(storage.memory && storage.wal_fd == -1 && storage.lock_fd == -1);
    CHECK(adb_storage_commit(&storage, state, &error));
    CHECK(adb_storage_checkpoint(&storage, state, &error));
    adb_storage_close(&storage); adb_state_free(state);
    CHECK(!adb_storage_open(&storage, "", &state, &error) && state == NULL);
}

int main(void) {
    codec_tests(); disk_tests(); memory_tests();
    printf("storage: %u assertions passed\n", assertions);
    return 0;
}
