#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void adb_column_free(ADBColumn *column)
{
    if (!column) return;
    free(column->name);
    adb_value_free(&column->default_value);
    memset(column, 0, sizeof(*column));
}

bool adb_column_copy(ADBColumn *dest, const ADBColumn *source, ADBError *error)
{
    memset(dest, 0, sizeof(*dest));
    dest->type = source->type;
    dest->primary_key = source->primary_key;
    dest->unique = source->unique;
    dest->not_null = source->not_null;
    dest->has_default = source->has_default;
    dest->name = adb_strdup(source->name, error);
    if (!dest->name) return false;
    if (!adb_value_copy(&dest->default_value, &source->default_value, error)) {
        adb_column_free(dest);
        return false;
    }
    return true;
}

void adb_table_free(ADBTable *table)
{
    if (!table) return;
    free(table->name);
    if (table->columns) {
        for (size_t c = 0; c < table->ncolumns; c++)
            adb_column_free(&table->columns[c]);
    }
    free(table->columns);
    if (table->rows) {
        for (size_t r = 0; r < table->nrows; r++) {
            if (table->rows[r].values) {
                for (size_t c = 0; c < table->ncolumns; c++)
                    adb_value_free(&table->rows[r].values[c]);
            }
            free(table->rows[r].values);
        }
    }
    free(table->rows);
    if (table->indexes) {
        for (size_t i = 0; i < table->nindexes; i++) {
            free(table->indexes[i].name);
            adb_tree_free(table->indexes[i].tree);
        }
    }
    free(table->indexes);
    memset(table, 0, sizeof(*table));
}

void adb_state_free(ADBState *state)
{
    if (!state) return;
    if (state->tables) {
        for (size_t t = 0; t < state->ntables; t++)
            adb_table_free(&state->tables[t]);
    }
    free(state->tables);
    free(state);
}

ADBState *adb_state_create(ADBError *error)
{
    return adb_alloc(1, sizeof(ADBState), error);
}

ADBTable *adb_find_table(ADBState *state, const char *name)
{
    if (!state || !name) return NULL;
    for (size_t t = 0; t < state->ntables; t++) {
        if (adb_name_equal(state->tables[t].name, name))
            return &state->tables[t];
    }
    return NULL;
}

int adb_find_column(const ADBTable *table, const char *name)
{
    if (!table || !name) return -1;
    for (size_t c = 0; c < table->ncolumns; c++) {
        if (adb_name_equal(table->columns[c].name, name)) return (int)c;
    }
    return -1;
}

static bool copy_table(ADBTable *dest, const ADBTable *source, ADBError *error)
{
    dest->name = adb_strdup(source->name, error);
    if (!dest->name) return false;
    dest->ncolumns = source->ncolumns;
    dest->next_row_id = source->next_row_id;
    dest->columns = adb_alloc(dest->ncolumns, sizeof(*dest->columns), error);
    if (!dest->columns) return false;
    for (size_t c = 0; c < dest->ncolumns; c++) {
        if (!adb_column_copy(&dest->columns[c], &source->columns[c], error))
            return false;
    }
    if (source->nrows) {
        dest->rows = adb_alloc(source->nrows, sizeof(*dest->rows), error);
        if (!dest->rows) return false;
        dest->row_capacity = dest->nrows = source->nrows;
        for (size_t r = 0; r < dest->nrows; r++) {
            dest->rows[r].id = source->rows[r].id;
            dest->rows[r].values = adb_alloc(dest->ncolumns,
                                            sizeof(*dest->rows[r].values), error);
            if (!dest->rows[r].values) return false;
            for (size_t c = 0; c < dest->ncolumns; c++) {
                if (!adb_value_copy(&dest->rows[r].values[c],
                                    &source->rows[r].values[c], error)) return false;
            }
        }
    }
    if (source->nindexes) {
        dest->indexes = adb_alloc(source->nindexes, sizeof(*dest->indexes), error);
        if (!dest->indexes) return false;
        dest->nindexes = source->nindexes;
        for (size_t i = 0; i < dest->nindexes; i++) {
            dest->indexes[i].name = adb_strdup(source->indexes[i].name, error);
            if (!dest->indexes[i].name) return false;
            dest->indexes[i].column = source->indexes[i].column;
            dest->indexes[i].unique = source->indexes[i].unique;
            dest->indexes[i].implicit = source->indexes[i].implicit;
        }
    }
    return adb_table_reindex(dest, error);
}

ADBState *adb_state_clone(const ADBState *state, ADBError *error)
{
    if (!state || state->ntables > ADB_MAX_TABLES) {
        adb_error(error, ADB_ERR_SCHEMA, "invalid database state");
        return NULL;
    }
    ADBState *copy = adb_state_create(error);
    if (!copy) return NULL;
    copy->version = state->version;
    if (state->ntables) {
        copy->tables = adb_alloc(state->ntables, sizeof(*copy->tables), error);
        if (!copy->tables) goto failure;
        copy->ntables = state->ntables;
        for (size_t t = 0; t < copy->ntables; t++) {
            if (!copy_table(&copy->tables[t], &state->tables[t], error)) goto failure;
        }
    }
    return copy;
failure:
    adb_state_free(copy);
    return NULL;
}

/* A private temporary tree enforces a column constraint even when a decoded
 * schema lacks its redundant implicit index definition. */
static bool validate_column_unique(const ADBTable *table, size_t column,
                                    ADBError *error)
{
    for (size_t i = 0; i < table->nindexes; i++) {
        if (table->indexes[i].column == column && table->indexes[i].unique)
            return true;
    }
    ADBTree *tree = adb_tree_create(error);
    if (!tree) return false;
    for (size_t r = 0; r < table->nrows; r++) {
        if (!adb_tree_insert(tree, &table->rows[r].values[column],
                             table->rows[r].id, true, error)) {
            adb_tree_free(tree);
            return false;
        }
    }
    adb_tree_free(tree);
    return true;
}

bool adb_table_reindex(ADBTable *table, ADBError *error)
{
    if (!table || !table->name || !table->ncolumns ||
        table->ncolumns > ADB_MAX_COLUMNS || !table->columns ||
        table->nrows > ADB_MAX_ROWS || (table->nrows && !table->rows) ||
        (table->nindexes && !table->indexes))
        return adb_error(error, ADB_ERR_SCHEMA, "invalid table layout");
    for (size_t c = 0; c < table->ncolumns; c++) {
        ADBColumn *column = &table->columns[c];
        if (!column->name || column->type < ADB_INTEGER || column->type > ADB_BOOLEAN)
            return adb_error(error, ADB_ERR_SCHEMA, "invalid column definition");
        for (size_t previous = 0; previous < c; previous++) {
            if (adb_name_equal(column->name, table->columns[previous].name))
                return adb_error(error, ADB_ERR_SCHEMA, "duplicate column '%s'", column->name);
        }
    }
    for (size_t r = 0; r < table->nrows; r++) {
        const ADBRow *row = &table->rows[r];
        if (!row->id || (r && row->id <= table->rows[r - 1].id))
            return adb_error(error, ADB_ERR_SCHEMA, "row IDs must be positive and strictly increasing");
        if (!row->values)
            return adb_error(error, ADB_ERR_SCHEMA, "missing row values");
        for (size_t c = 0; c < table->ncolumns; c++) {
            const ADBValue *value = &row->values[c];
            const ADBColumn *column = &table->columns[c];
            if (value->type == ADB_NULL) {
                if (column->not_null || column->primary_key)
                    return adb_error(error, ADB_ERR_CONSTRAINT,
                                     "column '%s' cannot be NULL", column->name);
            } else if (value->type != column->type) {
                return adb_error(error, ADB_ERR_TYPE,
                                 "type mismatch for column '%s'", column->name);
            } else if (value->type == ADB_REAL && !isfinite(value->as.real)) {
                return adb_error(error, ADB_ERR_TYPE,
                                 "non-finite value in column '%s'", column->name);
            } else if (value->type == ADB_TEXT &&
                       (!value->as.text || strlen(value->as.text) > ADB_MAX_TEXT)) {
                return adb_error(error, ADB_ERR_LIMIT,
                                 "invalid or oversized text in column '%s'", column->name);
            }
        }
    }
    if (!table->next_row_id ||
        (table->nrows && table->next_row_id <= table->rows[table->nrows - 1].id))
        return adb_error(error, ADB_ERR_SCHEMA, "invalid next row ID");
    for (size_t i = 0; i < table->nindexes; i++) {
        adb_tree_free(table->indexes[i].tree);
        table->indexes[i].tree = NULL;
    }
    for (size_t i = 0; i < table->nindexes; i++) {
        ADBIndex *index = &table->indexes[i];
        if (!index->name || index->column >= table->ncolumns)
            return adb_error(error, ADB_ERR_SCHEMA, "invalid index definition");
        index->tree = adb_tree_create(error);
        if (!index->tree) return false;
        for (size_t r = 0; r < table->nrows; r++) {
            if (!adb_tree_insert(index->tree, &table->rows[r].values[index->column],
                                 table->rows[r].id, index->unique, error)) return false;
        }
    }
    for (size_t c = 0; c < table->ncolumns; c++) {
        if ((table->columns[c].primary_key || table->columns[c].unique) &&
            !validate_column_unique(table, c, error)) return false;
    }
    return true;
}
