#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Format 1 is independent of struct layout and native endianness. Every frame
 * carries both a header CRC and a payload CRC. No pointers are serialized. */
#define FRAME_SIZE 32u
#define INDEX_LIMIT 4096u
static const unsigned char frame_magic[8] = {'A','T','L','A','S','D','B',0};

typedef struct { ADBBuffer bytes; ADBError *error; } Encoder;
typedef struct { const unsigned char *data; size_t length, offset; ADBError *error; bool failed; } Decoder;

uint32_t adb_crc32(const void *data, size_t length) {
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static void put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8u));
}
static void put64(unsigned char *p, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(n >> (i * 8u));
}
static uint32_t get32(const unsigned char *p) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)p[i] << (i * 8u);
    return n;
}
static uint64_t get64(const unsigned char *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= (uint64_t)p[i] << (i * 8u);
    return n;
}
static bool encode_bytes(Encoder *enc, const void *data, size_t n) {
    if (n > ADB_MAX_FRAME || enc->bytes.length > ADB_MAX_FRAME - n)
        return adb_error(enc->error, ADB_ERR_LIMIT, "database exceeds maximum frame size (%u bytes)", ADB_MAX_FRAME);
    if (!adb_buffer_append(&enc->bytes, data, n))
        return adb_error(enc->error, ADB_ERR_MEMORY, "cannot encode database state");
    return true;
}
static bool encode_u8(Encoder *enc, unsigned n) {
    unsigned char b = (unsigned char)n;
    return encode_bytes(enc, &b, 1);
}
static bool encode_u32(Encoder *enc, uint32_t n) {
    unsigned char b[4]; put32(b, n); return encode_bytes(enc, b, sizeof(b));
}
static bool encode_u64(Encoder *enc, uint64_t n) {
    unsigned char b[8]; put64(b, n); return encode_bytes(enc, b, sizeof(b));
}
static bool encode_string(Encoder *enc, const char *text) {
    if (!text) return adb_error(enc->error, ADB_ERR_CORRUPT, "cannot encode a missing string");
    size_t n = strlen(text);
    if (!adb_utf8_valid(text)) return adb_error(enc->error, ADB_ERR_TYPE, "stored strings must be valid UTF-8");
    if (n > ADB_MAX_TEXT) return adb_error(enc->error, ADB_ERR_LIMIT, "stored string exceeds size limit");
    return encode_u32(enc, (uint32_t)n) && encode_bytes(enc, text, n);
}
static bool encode_value(Encoder *enc, const ADBValue *value) {
    if (!encode_u8(enc, (unsigned)value->type)) return false;
    switch (value->type) {
    case ADB_NULL: return true;
    case ADB_INTEGER: return encode_u64(enc, (uint64_t)value->as.integer);
    case ADB_REAL: {
        uint64_t bits;
        if (sizeof(double) != 8 || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024 || !isfinite(value->as.real))
            return adb_error(enc->error, ADB_ERR_TYPE, "storage requires finite IEEE 754 binary64 numbers");
        memcpy(&bits, &value->as.real, sizeof(bits));
        return encode_u64(enc, bits);
    }
    case ADB_TEXT: return encode_string(enc, value->as.text);
    case ADB_BOOLEAN: return encode_u8(enc, value->as.boolean ? 1u : 0u);
    default: return adb_error(enc->error, ADB_ERR_CORRUPT, "unknown stored value type");
    }
}

bool adb_state_encode(const ADBState *state, unsigned char **data, size_t *length, ADBError *error) {
    Encoder enc = {{0}, error};
    *data = NULL; *length = 0;
    if (!state || state->ntables > ADB_MAX_TABLES)
        return adb_error(error, ADB_ERR_LIMIT, "invalid table count for storage");
    if (!encode_u64(&enc, state->version) || !encode_u32(&enc, (uint32_t)state->ntables)) goto fail;
    for (size_t ti = 0; ti < state->ntables; ++ti) {
        const ADBTable *table = &state->tables[ti];
        if (table->ncolumns == 0 || table->ncolumns > ADB_MAX_COLUMNS ||
            table->nrows > ADB_MAX_ROWS || table->nindexes > INDEX_LIMIT) {
            adb_error(error, ADB_ERR_LIMIT, "table exceeds storage limits"); goto fail;
        }
        if (!encode_string(&enc, table->name) || !encode_u32(&enc, (uint32_t)table->ncolumns)) goto fail;
        for (size_t ci = 0; ci < table->ncolumns; ++ci) {
            const ADBColumn *column = &table->columns[ci];
            unsigned flags = (column->primary_key ? 1u : 0u) | (column->unique ? 2u : 0u) |
                             (column->not_null ? 4u : 0u) | (column->has_default ? 8u : 0u);
            if (!encode_string(&enc, column->name) || !encode_u8(&enc, (unsigned)column->type) ||
                !encode_u8(&enc, flags) || !encode_value(&enc, &column->default_value)) goto fail;
        }
        if (!encode_u32(&enc, (uint32_t)table->nindexes)) goto fail;
        for (size_t ii = 0; ii < table->nindexes; ++ii) {
            const ADBIndex *index = &table->indexes[ii];
            if (index->column >= table->ncolumns) { adb_error(error, ADB_ERR_CORRUPT, "invalid index column"); goto fail; }
            if (!encode_string(&enc, index->name) || !encode_u32(&enc, (uint32_t)index->column) ||
                !encode_u8(&enc, index->unique ? 1u : 0u) || !encode_u8(&enc, index->implicit ? 1u : 0u)) goto fail;
        }
        if (!encode_u64(&enc, table->next_row_id) || !encode_u64(&enc, (uint64_t)table->nrows)) goto fail;
        for (size_t ri = 0; ri < table->nrows; ++ri) {
            if (!encode_u64(&enc, table->rows[ri].id)) goto fail;
            for (size_t ci = 0; ci < table->ncolumns; ++ci)
                if (!encode_value(&enc, &table->rows[ri].values[ci])) goto fail;
        }
    }
    *data = (unsigned char *)enc.bytes.data; *length = enc.bytes.length;
    return true;
fail:
    adb_buffer_free(&enc.bytes);
    return false;
}

static bool decode_fail(Decoder *dec, const char *message) {
    dec->failed = true;
    return adb_error(dec->error, ADB_ERR_CORRUPT, "invalid database payload at byte %zu: %s", dec->offset, message);
}
static const unsigned char *decode_bytes(Decoder *dec, size_t n) {
    if (dec->failed) return NULL;
    if (n > dec->length - dec->offset) { decode_fail(dec, "truncated field"); return NULL; }
    const unsigned char *p = dec->data + dec->offset;
    dec->offset += n;
    return p;
}
static unsigned decode_u8(Decoder *dec) {
    const unsigned char *p = decode_bytes(dec, 1); return p ? p[0] : 0;
}
static uint32_t decode_u32(Decoder *dec) {
    const unsigned char *p = decode_bytes(dec, 4); return p ? get32(p) : 0;
}
static uint64_t decode_u64(Decoder *dec) {
    const unsigned char *p = decode_bytes(dec, 8); return p ? get64(p) : 0;
}
static char *decode_string(Decoder *dec, bool name) {
    uint32_t n = decode_u32(dec);
    if (dec->failed) return NULL;
    if (n > ADB_MAX_TEXT || (name && n == 0)) { decode_fail(dec, "invalid string length"); return NULL; }
    const unsigned char *p = decode_bytes(dec, n);
    if (!p) return NULL;
    if (memchr(p, 0, n)) { decode_fail(dec, "embedded NUL byte"); return NULL; }
    char *text = adb_alloc((size_t)n + 1u, 1, dec->error);
    if (!text) { dec->failed = true; return NULL; }
    memcpy(text, p, n); text[n] = '\0';
    if (!adb_utf8_valid(text)) { free(text); decode_fail(dec, "invalid UTF-8 string"); return NULL; }
    return text;
}
static bool decode_value(Decoder *dec, ADBValue *value) {
    value->type = (ADBType)decode_u8(dec);
    if (dec->failed) return false;
    switch (value->type) {
    case ADB_NULL: break;
    case ADB_INTEGER: {
        uint64_t n = decode_u64(dec);
        value->as.integer = n <= INT64_MAX ? (int64_t)n : INT64_MIN + (int64_t)(n - (UINT64_C(1) << 63u));
        break;
    }
    case ADB_REAL: {
        uint64_t bits = decode_u64(dec);
        if (sizeof(double) != 8 || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024)
            return decode_fail(dec, "unsupported floating point format");
        memcpy(&value->as.real, &bits, sizeof(bits));
        if (!isfinite(value->as.real)) return decode_fail(dec, "non-finite number");
        break;
    }
    case ADB_TEXT:
        value->as.text = decode_string(dec, false);
        if (!value->as.text) return false;
        break;
    case ADB_BOOLEAN: {
        unsigned n = decode_u8(dec);
        if (n > 1) return decode_fail(dec, "invalid boolean");
        value->as.boolean = n != 0;
        break;
    }
    default:
        value->type = ADB_NULL;
        return decode_fail(dec, "unknown value type");
    }
    return !dec->failed;
}

static int compare_index_names(const void *left, const void *right) {
    const unsigned char *a = (const unsigned char *)*(const char *const *)left;
    const unsigned char *b = (const unsigned char *)*(const char *const *)right;
    while (*a && *b) {
        int folded_a = tolower(*a++), folded_b = tolower(*b++);
        if (folded_a != folded_b) return (folded_a > folded_b) - (folded_a < folded_b);
    }
    return (*a > *b) - (*a < *b);
}
static bool validate_index_names(Decoder *dec, const ADBState *state) {
    size_t count = 0;
    for (size_t ti = 0; ti < state->ntables; ++ti) count += state->tables[ti].nindexes;
    if (count < 2) return true;
    const char **names = adb_alloc(count, sizeof(*names), dec->error);
    if (!names) { dec->failed = true; return false; }
    size_t at = 0;
    for (size_t ti = 0; ti < state->ntables; ++ti)
        for (size_t ii = 0; ii < state->tables[ti].nindexes; ++ii)
            names[at++] = state->tables[ti].indexes[ii].name;
    qsort(names, count, sizeof(*names), compare_index_names);
    bool ok = true;
    for (size_t i = 1; i < count; ++i)
        if (adb_name_equal(names[i - 1], names[i])) { ok = false; break; }
    free(names);
    return ok || decode_fail(dec, "duplicate global index name");
}

ADBState *adb_state_decode(const unsigned char *data, size_t length, ADBError *error) {
    Decoder dec = {data, length, 0, error, false};
    ADBState *state = NULL;
    if (!data || length > ADB_MAX_FRAME) { adb_error(error, ADB_ERR_CORRUPT, "invalid database payload size"); return NULL; }
    state = adb_state_create(error);
    if (!state) return NULL;
    state->version = decode_u64(&dec);
    uint32_t ntables = decode_u32(&dec);
    if (dec.failed || ntables > ADB_MAX_TABLES || ntables > (length - dec.offset) / 33u) {
        if (!dec.failed) decode_fail(&dec, "invalid table count");
        goto fail;
    }
    if (ntables) {
        state->tables = adb_alloc(ntables, sizeof(*state->tables), error);
        if (!state->tables) goto fail;
    }
    state->ntables = ntables;
    for (size_t ti = 0; ti < state->ntables; ++ti) {
        ADBTable *table = &state->tables[ti];
        table->name = decode_string(&dec, true);
        if (!table->name) goto fail;
        for (size_t previous = 0; previous < ti; ++previous)
            if (adb_name_equal(state->tables[previous].name, table->name)) { decode_fail(&dec, "duplicate table name"); goto fail; }
        uint32_t ncolumns = decode_u32(&dec);
        if (dec.failed || ncolumns == 0 || ncolumns > ADB_MAX_COLUMNS || ncolumns > (length - dec.offset) / 8u) {
            if (!dec.failed) decode_fail(&dec, "invalid column count");
            goto fail;
        }
        table->columns = adb_alloc(ncolumns, sizeof(*table->columns), error);
        if (!table->columns) goto fail;
        table->ncolumns = ncolumns;
        unsigned primary_keys = 0;
        for (size_t ci = 0; ci < table->ncolumns; ++ci) {
            ADBColumn *column = &table->columns[ci];
            column->name = decode_string(&dec, true);
            if (!column->name) goto fail;
            for (size_t previous = 0; previous < ci; ++previous)
                if (adb_name_equal(table->columns[previous].name, column->name)) { decode_fail(&dec, "duplicate column name"); goto fail; }
            column->type = (ADBType)decode_u8(&dec);
            unsigned flags = decode_u8(&dec);
            if (dec.failed || column->type < ADB_INTEGER || column->type > ADB_BOOLEAN || flags > 15u) {
                if (!dec.failed) decode_fail(&dec, "invalid column definition");
                goto fail;
            }
            column->primary_key = (flags & 1u) != 0; column->unique = (flags & 2u) != 0;
            column->not_null = (flags & 4u) != 0; column->has_default = (flags & 8u) != 0;
            if (column->primary_key) ++primary_keys;
            if (primary_keys > 1) { decode_fail(&dec, "multiple primary keys"); goto fail; }
            if (!decode_value(&dec, &column->default_value)) goto fail;
            if (column->has_default && column->default_value.type != ADB_NULL && column->default_value.type != column->type) {
                decode_fail(&dec, "default type does not match column"); goto fail;
            }
        }
        uint32_t nindexes = decode_u32(&dec);
        if (dec.failed || nindexes > INDEX_LIMIT || nindexes > (length - dec.offset) / 11u) {
            if (!dec.failed) decode_fail(&dec, "invalid index count");
            goto fail;
        }
        if (nindexes) {
            table->indexes = adb_alloc(nindexes, sizeof(*table->indexes), error);
            if (!table->indexes) goto fail;
        }
        table->nindexes = nindexes;
        for (size_t ii = 0; ii < table->nindexes; ++ii) {
            ADBIndex *index = &table->indexes[ii];
            index->name = decode_string(&dec, true);
            index->column = decode_u32(&dec);
            unsigned unique = decode_u8(&dec), implicit = decode_u8(&dec);
            if (dec.failed || !index->name || index->column >= table->ncolumns || unique > 1 || implicit > 1) {
                if (!dec.failed) decode_fail(&dec, "invalid index definition");
                goto fail;
            }
            index->unique = unique != 0; index->implicit = implicit != 0;
        }
        table->next_row_id = decode_u64(&dec);
        uint64_t nrows = decode_u64(&dec);
        if (dec.failed || nrows > ADB_MAX_ROWS || nrows > (length - dec.offset) / (8u + table->ncolumns)) {
            if (!dec.failed) decode_fail(&dec, "invalid row count");
            goto fail;
        }
        if (nrows) {
            table->rows = adb_alloc((size_t)nrows, sizeof(*table->rows), error);
            if (!table->rows) goto fail;
        }
        table->row_capacity = table->nrows = (size_t)nrows;
        for (size_t ri = 0; ri < table->nrows; ++ri) {
            ADBRow *row = &table->rows[ri];
            row->id = decode_u64(&dec);
            if (dec.failed) goto fail;
            row->values = adb_alloc(table->ncolumns, sizeof(*row->values), error);
            if (!row->values) goto fail;
            for (size_t ci = 0; ci < table->ncolumns; ++ci)
                if (!decode_value(&dec, &row->values[ci])) goto fail;
        }
        if (!adb_table_reindex(table, error)) {
            if (error && error->code != ADB_ERR_MEMORY) error->code = ADB_ERR_CORRUPT;
            goto fail;
        }
    }
    if (dec.failed || dec.offset != length) { if (!dec.failed) decode_fail(&dec, "trailing payload bytes"); goto fail; }
    if (!validate_index_names(&dec, state)) goto fail;
    return state;
fail:
    adb_state_free(state);
    return NULL;
}

static char *storage_path(const ADBStorage *storage, const char *file, ADBError *error) {
    size_t base = strlen(storage->directory), suffix = strlen(file);
    if (base > SIZE_MAX - suffix - 2u) { adb_error(error, ADB_ERR_LIMIT, "database path is too long"); return NULL; }
    char *path = adb_alloc(base + suffix + 2u, 1, error);
    if (!path) return NULL;
    memcpy(path, storage->directory, base); path[base] = '/';
    memcpy(path + base + 1u, file, suffix + 1u);
    return path;
}
static bool io_error(ADBError *error, const char *operation) {
    int saved = errno;
    return adb_error(error, ADB_ERR_IO, "%s: %s", operation, strerror(saved));
}
static bool write_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) { if (errno == EINTR) continue; return false; }
        if (!written) { errno = EIO; return false; }
        bytes += (size_t)written; length -= (size_t)written;
    }
    return true;
}
/* false indicates IO error; a short successful read is reported through count. */
static bool read_some(int fd, void *data, size_t length, size_t *count) {
    unsigned char *bytes = data; *count = 0;
    while (*count < length) {
        ssize_t n = read(fd, bytes + *count, length - *count);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (!n) break;
        *count += (size_t)n;
    }
    return true;
}
static bool sync_file(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}
static bool sync_directory(const ADBStorage *storage, ADBError *error) {
#ifdef _WIN32
    /* Windows has no POSIX directory fsync. File commits use _commit and
     * checkpoint publication uses MoveFileExW with MOVEFILE_WRITE_THROUGH. */
    (void)storage; (void)error;
    return true;
#else
    int fd = open(storage->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return io_error(error, "open database directory");
    bool ok = sync_file(fd);
    int saved = errno; close(fd); errno = saved;
    return ok || io_error(error, "sync database directory");
#endif
}
/* Creating a new database directory also changes its parent directory. */
static bool sync_parent_directory(const ADBStorage *storage, ADBError *error) {
#ifdef _WIN32
    (void)storage; (void)error;
    return true;
#else
    char *parent = adb_strdup(storage->directory, error);
    if (!parent) return false;
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1] == '/') parent[--length] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash) { free(parent); parent = adb_strdup(".", error); if (!parent) return false; }
    else if (slash == parent) parent[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    if (fd < 0) return io_error(error, "open parent directory");
    bool ok = sync_file(fd);
    int saved = errno; close(fd); errno = saved;
    return ok || io_error(error, "sync parent directory");
#endif
}
static void frame_header(unsigned char header[FRAME_SIZE], const unsigned char *payload, size_t length) {
    memcpy(header, frame_magic, sizeof(frame_magic));
    put32(header + 8, 1); put32(header + 12, FRAME_SIZE); put64(header + 16, length);
    put32(header + 24, adb_crc32(payload, length)); put32(header + 28, adb_crc32(header, 28));
}
static bool frame_length(const unsigned char header[FRAME_SIZE], size_t *length, ADBError *error) {
    if (memcmp(header, frame_magic, sizeof(frame_magic)) || get32(header + 8) != 1 ||
        get32(header + 12) != FRAME_SIZE || get32(header + 28) != adb_crc32(header, 28))
        return adb_error(error, ADB_ERR_CORRUPT, "invalid database frame header or checksum");
    uint64_t n = get64(header + 16);
    if (n < 12u || n > ADB_MAX_FRAME)
        return adb_error(error, ADB_ERR_CORRUPT, "invalid database frame length");
    *length = (size_t)n;
    return true;
}

static bool recover_file(int fd, bool snapshot, ADBState **state, off_t *valid_end, ADBError *error) {
    off_t position = 0;
    bool seen = false;
    uint64_t previous_version = 0;
    *valid_end = 0;
    for (;;) {
        unsigned char header[FRAME_SIZE]; size_t count = 0;
        if (!read_some(fd, header, sizeof(header), &count)) return io_error(error, "read database frame");
        if (!count) break;
        if (snapshot && seen) return adb_error(error, ADB_ERR_CORRUPT, "snapshot contains trailing bytes");
        if (count != sizeof(header)) {
            if (snapshot) return adb_error(error, ADB_ERR_CORRUPT, "snapshot frame is incomplete");
            break; /* WAL torn final header */
        }
        size_t length = 0;
        if (!frame_length(header, &length, error)) return false;
        unsigned char *payload = adb_alloc(length, 1, error);
        if (!payload) return false;
        bool read_ok = read_some(fd, payload, length, &count);
        if (!read_ok) { free(payload); return io_error(error, "read database payload"); }
        if (count != length) {
            free(payload);
            if (snapshot) return adb_error(error, ADB_ERR_CORRUPT, "snapshot payload is incomplete");
            break; /* WAL torn final payload */
        }
        if (get32(header + 24) != adb_crc32(payload, length)) {
            free(payload); return adb_error(error, ADB_ERR_CORRUPT, "database payload checksum mismatch");
        }
        ADBState *candidate = adb_state_decode(payload, length, error);
        free(payload);
        if (!candidate) return false;
        if (seen && candidate->version <= previous_version) {
            adb_state_free(candidate);
            return adb_error(error, ADB_ERR_CORRUPT, "WAL versions are not strictly increasing");
        }
        previous_version = candidate->version; seen = true;
        if (!*state || candidate->version > (*state)->version) {
            adb_state_free(*state); *state = candidate;
        } else adb_state_free(candidate);
        if (length > (size_t)INT64_MAX - FRAME_SIZE || position > (off_t)(INT64_MAX - (int64_t)length - FRAME_SIZE))
            return adb_error(error, ADB_ERR_LIMIT, "database file offset overflow");
        position += (off_t)(FRAME_SIZE + length);
        *valid_end = position;
    }
    if (snapshot && !seen) return adb_error(error, ADB_ERR_CORRUPT, "snapshot is empty");
    return true;
}

bool adb_storage_open(ADBStorage *storage, const char *directory, ADBState **state, ADBError *error) {
    memset(storage, 0, sizeof(*storage)); storage->lock_fd = storage->wal_fd = -1;
    *state = NULL;
    if (!directory || !*directory) return adb_error(error, ADB_ERR_IO, "database directory must not be empty");
    storage->memory = strcmp(directory, ":memory:") == 0;
    if (storage->memory) { *state = adb_state_create(error); return *state != NULL; }
    storage->directory = adb_strdup(directory, error);
    if (!storage->directory) return false;
    bool created = mkdir(directory, 0700) == 0;
    if (!created && errno != EEXIST) { io_error(error, "create database directory"); goto fail; }
    struct stat info;
    if (stat(directory, &info) < 0) { io_error(error, "inspect database directory"); goto fail; }
    if (!S_ISDIR(info.st_mode)) { adb_error(error, ADB_ERR_IO, "database path is not a directory"); goto fail; }
    char *path = storage_path(storage, "atlas.lock", error);
    if (!path) goto fail;
    storage->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600); free(path);
    if (storage->lock_fd < 0) { io_error(error, "open database lock"); goto fail; }
    if (flock(storage->lock_fd, LOCK_EX | LOCK_NB) < 0) { io_error(error, "database directory is already locked"); goto fail; }
    path = storage_path(storage, "atlas.snapshot", error);
    if (!path) goto fail;
    int snapshot_fd = open(path, O_RDONLY | O_CLOEXEC); free(path);
    if (snapshot_fd < 0 && errno != ENOENT) { io_error(error, "open snapshot"); goto fail; }
    if (snapshot_fd >= 0) {
        off_t valid_end;
        bool ok = recover_file(snapshot_fd, true, state, &valid_end, error);
        close(snapshot_fd);
        if (!ok) goto fail;
    }
    path = storage_path(storage, "atlas.wal", error);
    if (!path) goto fail;
    storage->wal_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600); free(path);
    if (storage->wal_fd < 0) { io_error(error, "open WAL"); goto fail; }
    off_t valid_end = 0;
    if (!recover_file(storage->wal_fd, false, state, &valid_end, error)) goto fail;
    if (fstat(storage->wal_fd, &info) < 0) { io_error(error, "inspect WAL"); goto fail; }
    if (info.st_size != valid_end) {
        if (ftruncate(storage->wal_fd, valid_end) < 0 || !sync_file(storage->wal_fd)) { io_error(error, "truncate incomplete WAL tail"); goto fail; }
    }
    if (lseek(storage->wal_fd, 0, SEEK_END) < 0) { io_error(error, "seek WAL"); goto fail; }
    if (!sync_directory(storage, error)) goto fail;
    if (created && !sync_parent_directory(storage, error)) goto fail;
    if (!*state) { *state = adb_state_create(error); if (!*state) goto fail; }
    return true;
fail:
    adb_state_free(*state); *state = NULL;
    adb_storage_close(storage);
    return false;
}

void adb_storage_close(ADBStorage *storage) {
    if (!storage) return;
    if (storage->wal_fd >= 0) close(storage->wal_fd);
    if (storage->lock_fd >= 0) close(storage->lock_fd);
    free(storage->directory);
    memset(storage, 0, sizeof(*storage)); storage->lock_fd = storage->wal_fd = -1;
}

bool adb_storage_commit(ADBStorage *storage, const ADBState *state, ADBError *error) {
    if (storage->memory) return true;
    if (storage->wal_fd < 0) return adb_error(error, ADB_ERR_IO, "storage is unavailable; reopen the database");
    unsigned char *payload = NULL, header[FRAME_SIZE]; size_t length = 0;
    if (!adb_state_encode(state, &payload, &length, error)) return false;
    frame_header(header, payload, length);
    off_t start = lseek(storage->wal_fd, 0, SEEK_END);
    if (start < 0) { free(payload); return io_error(error, "seek WAL before commit"); }
    bool ok = write_all(storage->wal_fd, header, sizeof(header)) && write_all(storage->wal_fd, payload, length) && sync_file(storage->wal_fd);
    int saved = errno; free(payload);
    if (ok) return true;
    /* If append/fsync fails, remove the attempted record before permitting any
     * further write. Failure of this rollback poisons the backend; durability
     * after a hard IO failure cannot be inferred by the in-memory caller. */
    bool restored = ftruncate(storage->wal_fd, start) == 0 && sync_file(storage->wal_fd) && lseek(storage->wal_fd, start, SEEK_SET) >= 0;
    if (!restored) {
        close(storage->wal_fd); storage->wal_fd = -1;
        return adb_error(error, ADB_ERR_IO, "WAL write and durable rollback failed; commit outcome is uncertain, reopen database");
    }
    errno = saved;
    return io_error(error, "commit WAL record");
}

bool adb_storage_checkpoint(ADBStorage *storage, const ADBState *state, ADBError *error) {
    if (storage->memory) return true;
    if (storage->wal_fd < 0) return adb_error(error, ADB_ERR_IO, "storage is unavailable; reopen the database");
    char *temporary = storage_path(storage, "atlas.snapshot.tmp", error);
    char *destination = storage_path(storage, "atlas.snapshot", error);
    unsigned char *payload = NULL, header[FRAME_SIZE]; size_t length = 0;
    int fd = -1;
    bool ok = false;
    if (!temporary || !destination || !adb_state_encode(state, &payload, &length, error)) goto done;
    frame_header(header, payload, length);
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { io_error(error, "create checkpoint file"); goto done; }
    if (!write_all(fd, header, sizeof(header)) || !write_all(fd, payload, length) || !sync_file(fd)) {
        io_error(error, "write checkpoint file"); goto done;
    }
    if (close(fd) < 0) { fd = -1; io_error(error, "close checkpoint file"); goto done; }
    fd = -1;
    if (rename(temporary, destination) < 0) { io_error(error, "publish checkpoint"); goto done; }
    /* The directory barrier makes the snapshot durable before WAL destruction.
     * A crash before WAL truncation may leave redundant records, which recovery
     * safely ignores when their versions do not exceed the snapshot version. */
    if (!sync_directory(storage, error)) goto done;
    if (ftruncate(storage->wal_fd, 0) < 0 || !sync_file(storage->wal_fd) || lseek(storage->wal_fd, 0, SEEK_SET) < 0) {
        io_error(error, "reset WAL after checkpoint"); goto done;
    }
    ok = true;
done:
    if (fd >= 0) close(fd);
    if (!ok && temporary) unlink(temporary);
    free(temporary); free(destination); free(payload);
    return ok;
}
