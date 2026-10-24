#include "internal.h"
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool adb_error(ADBError *error, ADBCode code, const char *format, ...) {
    if (error) {
        error->code = code;
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return false;
}

void *adb_alloc(size_t count, size_t size, ADBError *error) {
    if (size && count > SIZE_MAX / size) {
        adb_error(error, ADB_ERR_LIMIT, "Allocation size overflow");
        return NULL;
    }
    void *memory = calloc(count ? count : 1, size ? size : 1);
    if (!memory) adb_error(error, ADB_ERR_MEMORY, "Out of memory");
    return memory;
}

char *adb_strdup(const char *text, ADBError *error) {
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = adb_alloc(size, 1, error);
    if (copy) memcpy(copy, text, size);
    return copy;
}

bool adb_name_equal(const char *left, const char *right) {
    if (!left || !right) return left == right;
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return false;
    }
    return *left == *right;
}

bool adb_utf8_valid(const char *text) {
    const unsigned char *bytes = (const unsigned char *)text;
    size_t length = strlen(text);
    for (size_t i = 0; i < length;) {
        unsigned char c = bytes[i];
        if (c < 0x80) { i++; continue; }
        size_t count = c >= 0xc2 && c <= 0xdf ? 2 :
                       c >= 0xe0 && c <= 0xef ? 3 :
                       c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!count || count > length - i) return false;
        for (size_t j = 1; j < count; j++)
            if ((bytes[i + j] & 0xc0) != 0x80) return false;
        if ((c == 0xe0 && bytes[i + 1] < 0xa0) ||
            (c == 0xed && bytes[i + 1] >= 0xa0) ||
            (c == 0xf0 && bytes[i + 1] < 0x90) ||
            (c == 0xf4 && bytes[i + 1] >= 0x90)) return false;
        i += count;
    }
    return true;
}

bool adb_buffer_append(ADBBuffer *buffer, const void *data, size_t length) {
    if (buffer->failed) return false;
    if (length > SIZE_MAX - buffer->length - 1) { buffer->failed = true; return false; }
    size_t required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        size_t cap = buffer->capacity ? buffer->capacity : 256;
        while (cap < required) {
            if (cap > SIZE_MAX / 2) { cap = required; break; }
            cap *= 2;
        }
        char *grown = realloc(buffer->data, cap);
        if (!grown) { buffer->failed = true; return false; }
        buffer->data = grown;
        buffer->capacity = cap;
    }
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool adb_buffer_printf(ADBBuffer *buffer, const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) { va_end(args); buffer->failed = true; return false; }
    char *text = malloc((size_t)length + 1);
    if (!text) { va_end(args); buffer->failed = true; return false; }
    vsnprintf(text, (size_t)length + 1, format, args);
    va_end(args);
    bool ok = adb_buffer_append(buffer, text, (size_t)length);
    free(text);
    return ok;
}

bool adb_buffer_json_string(ADBBuffer *buffer, const char *text) {
    if (!adb_buffer_append(buffer, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"': adb_buffer_append(buffer, "\\\"", 2); break;
        case '\\': adb_buffer_append(buffer, "\\\\", 2); break;
        case '\b': adb_buffer_append(buffer, "\\b", 2); break;
        case '\f': adb_buffer_append(buffer, "\\f", 2); break;
        case '\n': adb_buffer_append(buffer, "\\n", 2); break;
        case '\r': adb_buffer_append(buffer, "\\r", 2); break;
        case '\t': adb_buffer_append(buffer, "\\t", 2); break;
        default:
            if (*p < 32) adb_buffer_printf(buffer, "\\u%04x", (unsigned)*p);
            else adb_buffer_append(buffer, p, 1);
        }
        if (buffer->failed) return false;
    }
    return adb_buffer_append(buffer, "\"", 1);
}

void adb_buffer_free(ADBBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

ADBValue adb_null(void) { ADBValue value = {0}; return value; }

const char *adb_type_name(ADBType type) {
    switch (type) {
    case ADB_NULL: return "NULL";
    case ADB_INTEGER: return "INTEGER";
    case ADB_REAL: return "REAL";
    case ADB_TEXT: return "TEXT";
    case ADB_BOOLEAN: return "BOOLEAN";
    }
    return "UNKNOWN";
}

void adb_value_free(ADBValue *value) {
    if (!value) return;
    if (value->type == ADB_TEXT) free(value->as.text);
    *value = adb_null();
}

bool adb_value_copy(ADBValue *dest, const ADBValue *source, ADBError *error) {
    *dest = *source;
    if (source->type == ADB_TEXT) {
        dest->as.text = adb_strdup(source->as.text ? source->as.text : "", error);
        if (!dest->as.text) { *dest = adb_null(); return false; }
    }
    return true;
}

static int compare_integer_real(int64_t integer, double real) {
    if (isnan(real)) return -1;
    if (real >= 9223372036854775808.0) return -1;
    if (real < -9223372036854775808.0) return 1;
    int64_t truncated = (int64_t)real;
    if (integer < truncated) return -1;
    if (integer > truncated) return 1;
    double fraction = real - (double)truncated;
    return fraction > 0 ? -1 : fraction < 0 ? 1 : 0;
}

int adb_value_compare(const ADBValue *a, const ADBValue *b) {
    if (a->type == ADB_NULL || b->type == ADB_NULL)
        return (a->type != ADB_NULL) - (b->type != ADB_NULL);
    if (a->type == ADB_INTEGER && b->type == ADB_REAL) return compare_integer_real(a->as.integer, b->as.real);
    if (a->type == ADB_REAL && b->type == ADB_INTEGER) return -compare_integer_real(b->as.integer, a->as.real);
    if (a->type != b->type) return (a->type > b->type) - (a->type < b->type);
    switch (a->type) {
    case ADB_INTEGER: return (a->as.integer > b->as.integer) - (a->as.integer < b->as.integer);
    case ADB_REAL:
        if (isnan(a->as.real) || isnan(b->as.real))
            return (int)isnan(a->as.real) - (int)isnan(b->as.real);
        return (a->as.real > b->as.real) - (a->as.real < b->as.real);
    case ADB_TEXT: return strcmp(a->as.text, b->as.text);
    case ADB_BOOLEAN: return (int)a->as.boolean - (int)b->as.boolean;
    case ADB_NULL: return 0;
    }
    return 0;
}

bool adb_value_coerce(ADBValue *value, ADBType type, ADBError *error) {
    if (value->type == ADB_NULL || value->type == type) return true;
    if (type == ADB_REAL && value->type == ADB_INTEGER) {
        double real = (double)value->as.integer;
        value->type = ADB_REAL;
        value->as.real = real;
        return true;
    }
    if (type == ADB_INTEGER && value->type == ADB_REAL && isfinite(value->as.real) &&
        value->as.real >= -9223372036854775808.0 && value->as.real < 9223372036854775808.0) {
        int64_t integer = (int64_t)value->as.real;
        if ((double)integer == value->as.real) {
            value->type = ADB_INTEGER;
            value->as.integer = integer;
            return true;
        }
    }
    if (type == ADB_BOOLEAN && value->type == ADB_INTEGER &&
        (value->as.integer == 0 || value->as.integer == 1)) {
        bool boolean = value->as.integer != 0;
        value->type = ADB_BOOLEAN;
        value->as.boolean = boolean;
        return true;
    }
    return adb_error(error, ADB_ERR_TYPE, "Cannot assign %s to %s", adb_type_name(value->type), adb_type_name(type));
}

bool adb_value_truth(const ADBValue *value) {
    switch (value->type) {
    case ADB_NULL: return false;
    case ADB_INTEGER: return value->as.integer != 0;
    case ADB_REAL: return value->as.real != 0;
    case ADB_BOOLEAN: return value->as.boolean;
    case ADB_TEXT: return value->as.text && *value->as.text;
    }
    return false;
}

char *adb_value_string(const ADBValue *value) {
    char buffer[128];
    switch (value->type) {
    case ADB_NULL: return adb_strdup("NULL", NULL);
    case ADB_TEXT: return adb_strdup(value->as.text, NULL);
    case ADB_BOOLEAN: return adb_strdup(value->as.boolean ? "true" : "false", NULL);
    case ADB_INTEGER: snprintf(buffer, sizeof(buffer), "%" PRId64, value->as.integer); break;
    case ADB_REAL: snprintf(buffer, sizeof(buffer), "%.17g", value->as.real); break;
    default: return NULL;
    }
    return adb_strdup(buffer, NULL);
}

void adb_result_free(ADBResult *result) {
    if (!result) return;
    for (size_t i = 0; i < result->ncolumns; i++) free(result->columns[i]);
    for (size_t i = 0; i < result->nrows * result->ncolumns; i++) adb_value_free(&result->cells[i]);
    free(result->columns);
    free(result->cells);
    free(result->message);
    memset(result, 0, sizeof(*result));
}

bool adb_result_columns(ADBResult *result, size_t count, const char *const *names, ADBError *error) {
    if (result->columns || result->cells) return adb_error(error, ADB_ERR_SCHEMA, "Result columns already initialized");
    if (count > ADB_MAX_COLUMNS * 4) return adb_error(error, ADB_ERR_LIMIT, "Too many result columns");
    result->columns = adb_alloc(count, sizeof(char *), error);
    if (!result->columns) return false;
    result->ncolumns = count;
    for (size_t i = 0; i < count; i++) {
        result->columns[i] = adb_strdup(names[i], error);
        if (!result->columns[i]) return false;
    }
    return true;
}

bool adb_result_row(ADBResult *result, const ADBValue *values, ADBError *error) {
    if (!result->ncolumns) return adb_error(error, ADB_ERR_SCHEMA, "No result columns");
    size_t row_text = 0;
    for (size_t i = 0; i < result->ncolumns; i++) {
        if (values[i].type != ADB_TEXT) continue;
        size_t bytes = strlen(values[i].as.text) + 1;
        if (bytes > ADB_MAX_RESULT_BYTES - row_text)
            return adb_error(error, ADB_ERR_LIMIT, "Result text limit exceeded (64 MiB)");
        row_text += bytes;
    }
    if (row_text > ADB_MAX_RESULT_BYTES - result->text_bytes)
        return adb_error(error, ADB_ERR_LIMIT, "Result text limit exceeded (64 MiB)");
    size_t total_text = result->text_bytes + row_text;
    if (result->nrows == result->capacity) {
        size_t capacity = result->capacity ? result->capacity * 2 : 16;
        if (capacity > ADB_MAX_ROWS || capacity > (ADB_MAX_RESULT_BYTES - total_text) / sizeof(ADBValue) / result->ncolumns) {
            capacity = result->nrows + 1;
            if (capacity > ADB_MAX_ROWS || capacity > (ADB_MAX_RESULT_BYTES - total_text) / sizeof(ADBValue) / result->ncolumns)
                return adb_error(error, ADB_ERR_LIMIT, "Result size limit exceeded");
        }
        ADBValue *cells = realloc(result->cells, capacity * result->ncolumns * sizeof(*cells));
        if (!cells) return adb_error(error, ADB_ERR_MEMORY, "Out of memory");
        result->cells = cells;
        result->capacity = capacity;
    }
    if (result->capacity > (ADB_MAX_RESULT_BYTES - total_text) / sizeof(ADBValue) / result->ncolumns)
        return adb_error(error, ADB_ERR_LIMIT, "Result size limit exceeded (64 MiB)");
    ADBValue *row = result->cells + result->nrows * result->ncolumns;
    memset(row, 0, result->ncolumns * sizeof(*row));
    for (size_t i = 0; i < result->ncolumns; i++) {
        if (!adb_value_copy(&row[i], &values[i], error)) {
            for (size_t j = 0; j < i; j++) adb_value_free(&row[j]);
            return false;
        }
    }
    result->nrows++;
    result->text_bytes = total_text;
    return true;
}

bool adb_result_message(ADBResult *result, const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) { va_end(args); return false; }
    char *message = malloc((size_t)length + 1);
    if (!message) { va_end(args); return false; }
    vsnprintf(message, (size_t)length + 1, format, args);
    va_end(args);
    free(result->message);
    result->message = message;
    return true;
}

char *adb_result_json(const ADBResult *result) {
    ADBBuffer buffer = {0};
    adb_buffer_append(&buffer, "{\"columns\":[", 12);
    for (size_t i = 0; i < result->ncolumns; i++) {
        if (i) adb_buffer_append(&buffer, ",", 1);
        adb_buffer_json_string(&buffer, result->columns[i]);
    }
    adb_buffer_append(&buffer, "],\"rows\":[", 10);
    for (size_t r = 0; r < result->nrows; r++) {
        if (r) adb_buffer_append(&buffer, ",", 1);
        adb_buffer_append(&buffer, "[", 1);
        for (size_t c = 0; c < result->ncolumns; c++) {
            if (c) adb_buffer_append(&buffer, ",", 1);
            const ADBValue *value = &result->cells[r * result->ncolumns + c];
            switch (value->type) {
            case ADB_NULL: adb_buffer_append(&buffer, "null", 4); break;
            case ADB_TEXT: adb_buffer_json_string(&buffer, value->as.text); break;
            case ADB_INTEGER: adb_buffer_printf(&buffer, "%" PRId64, value->as.integer); break;
            case ADB_REAL:
                if (isfinite(value->as.real)) adb_buffer_printf(&buffer, "%.17g", value->as.real);
                else adb_buffer_append(&buffer, "null", 4);
                break;
            case ADB_BOOLEAN: adb_buffer_append(&buffer, value->as.boolean ? "true" : "false", value->as.boolean ? 4 : 5); break;
            }
        }
        adb_buffer_append(&buffer, "]", 1);
    }
    adb_buffer_printf(&buffer, "],\"affected\":%" PRIu64 ",\"message\":", result->affected);
    if (result->message) adb_buffer_json_string(&buffer, result->message);
    else adb_buffer_append(&buffer, "null", 4);
    adb_buffer_append(&buffer, "}", 1);
    if (buffer.failed) { adb_buffer_free(&buffer); return NULL; }
    return buffer.data;
}
