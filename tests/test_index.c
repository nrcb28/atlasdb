#include "internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { STRESS_ROWS = 20000 };

static ADBValue integer(int64_t value)
{
    ADBValue result = {.type = ADB_INTEGER};
    result.as.integer = value;
    return result;
}

static ADBValue real(double value)
{
    ADBValue result = {.type = ADB_REAL};
    result.as.real = value;
    return result;
}

static void compare_both(const ADBValue *a, const ADBValue *b, int expected)
{
    int forward = adb_value_compare(a, b);
    int backward = adb_value_compare(b, a);
    CHECK(((forward > 0) - (forward < 0)) == expected);
    CHECK(((backward > 0) - (backward < 0)) == -expected);
}

static void numeric_boundaries(void)
{
    ADBValue a = integer(INT64_MAX), b = real(9223372036854775808.0);
    compare_both(&a, &b, -1);
    a = integer(INT64_MIN);
    b = real(-9223372036854775808.0);
    compare_both(&a, &b, 0);
    b = real(nextafter(-9223372036854775808.0, -INFINITY));
    compare_both(&a, &b, 1);
    a = integer(INT64_C(9007199254740993));
    b = real(9007199254740992.0);
    compare_both(&a, &b, 1);
    a = integer(-INT64_C(9007199254740993));
    b = real(-9007199254740992.0);
    compare_both(&a, &b, -1);
    a = integer(0);
    b = real(0.5);
    compare_both(&a, &b, -1);
    b = real(-0.5);
    compare_both(&a, &b, 1);
    b = real(-0.0);
    compare_both(&a, &b, 0);
    b = adb_null();
    compare_both(&a, &b, 1);

    ADBError error = {0};
    ADBTree *tree = adb_tree_create(&error);
    CHECK(tree);
    a = integer(42);
    b = real(42.0);
    CHECK(adb_tree_insert(tree, &a, 1, true, &error));
    CHECK(!adb_tree_insert(tree, &b, 2, true, &error));
    CHECK(error.code == ADB_ERR_CONSTRAINT);
    size_t count;
    const uint64_t *rows = adb_tree_find(tree, &b, &count);
    CHECK(rows && count == 1 && rows[0] == 1);
    adb_tree_free(tree);
}

/* Monotonic insertion stresses balancing; permuted insertion exercises both
 * double rotations. Every lookup also verifies its stable row identifier. */
static void stress_index(unsigned mode)
{
    ADBError error = {0};
    ADBTree *tree = adb_tree_create(&error);
    CHECK(tree);
    for (int n = 0; n < STRESS_ROWS; n++) {
        int key = mode == 0 ? n : mode == 1 ? STRESS_ROWS - 1 - n :
                  (n * 7919) % STRESS_ROWS;
        ADBValue value = integer(key);
        CHECK(adb_tree_insert(tree, &value, (uint64_t)key + 1, true, &error));
    }
    for (int key = 0; key < STRESS_ROWS; key++) {
        ADBValue value = integer(key);
        size_t count = 0;
        const uint64_t *rows = adb_tree_find(tree, &value, &count);
        CHECK(rows && count == 1 && rows[0] == (uint64_t)key + 1);
    }
    ADBValue value = integer(-1);
    size_t count = 99;
    CHECK(!adb_tree_find(tree, &value, &count) && count == 0);
    value = integer(100);
    CHECK(!adb_tree_insert(tree, &value, STRESS_ROWS + 1, true, &error));
    CHECK(error.code == ADB_ERR_CONSTRAINT);
    CHECK(adb_tree_insert(tree, &value, STRESS_ROWS + 1, false, &error));
    const uint64_t *rows = adb_tree_find(tree, &value, &count);
    CHECK(rows && count == 2 && rows[0] == 101 && rows[1] == STRESS_ROWS + 1);

    value = adb_null();
    for (uint64_t id = STRESS_ROWS + 2; id < STRESS_ROWS + 102; id++)
        CHECK(adb_tree_insert(tree, &value, id, true, &error));
    rows = adb_tree_find(tree, &value, &count);
    CHECK(rows && count == 100 && rows[0] == STRESS_ROWS + 2 &&
          rows[99] == STRESS_ROWS + 101);

    value.type = ADB_TEXT;
    value.as.text = adb_strdup("independently owned index key", &error);
    CHECK(value.as.text);
    CHECK(adb_tree_insert(tree, &value, STRESS_ROWS + 102, true, &error));
    adb_value_free(&value);
    value = (ADBValue){.type = ADB_TEXT, .as.text = "independently owned index key"};
    rows = adb_tree_find(tree, &value, &count);
    CHECK(rows && count == 1 && rows[0] == STRESS_ROWS + 102);
    adb_tree_free(tree);
}

static ADBState *sample_state(ADBError *error)
{
    ADBState *state = adb_state_create(error);
    CHECK(state);
    state->version = 42;
    state->tables = adb_alloc(1, sizeof(*state->tables), error);
    CHECK(state->tables);
    state->ntables = 1;
    ADBTable *table = &state->tables[0];
    table->name = adb_strdup("People", error);
    CHECK(table->name);
    table->ncolumns = 2;
    table->columns = adb_alloc(table->ncolumns, sizeof(*table->columns), error);
    CHECK(table->columns);
    table->columns[0] = (ADBColumn){.name = adb_strdup("id", error),
                                   .type = ADB_INTEGER, .primary_key = true};
    table->columns[1] = (ADBColumn){.name = adb_strdup("name", error),
                                   .type = ADB_TEXT, .unique = true, .has_default = true,
                                   .default_value = {.type = ADB_TEXT,
                                                     .as.text = adb_strdup("Anonymous", error)}};
    CHECK(table->columns[0].name && table->columns[1].name &&
          table->columns[1].default_value.as.text);
    table->rows = adb_alloc(3, sizeof(*table->rows), error);
    CHECK(table->rows);
    table->row_capacity = table->nrows = 3;
    table->next_row_id = 4;
    for (size_t r = 0; r < table->nrows; r++) {
        table->rows[r].id = r + 1;
        table->rows[r].values = adb_alloc(table->ncolumns, sizeof(ADBValue), error);
        CHECK(table->rows[r].values);
        table->rows[r].values[0] = integer((int64_t)r + 1);
    }
    table->rows[0].values[1] = (ADBValue){.type = ADB_TEXT,
                                        .as.text = adb_strdup("Ada", error)};
    CHECK(table->rows[0].values[1].as.text);
    table->indexes = adb_alloc(1, sizeof(*table->indexes), error);
    CHECK(table->indexes);
    table->nindexes = 1;
    table->indexes[0] = (ADBIndex){.name = adb_strdup("idx_name", error), .column = 1};
    CHECK(table->indexes[0].name);
    return state;
}

static void model_integrity(void)
{
    ADBError error = {0};
    ADBState *source = sample_state(&error);
    ADBTable *table = &source->tables[0];
    CHECK(adb_table_reindex(table, &error));
    CHECK(adb_find_table(source, "PEOPLE") == table);
    CHECK(adb_find_column(table, "NAME") == 1);
    CHECK(adb_find_column(table, "missing") == -1);
    ADBState *copy = adb_state_clone(source, &error);
    CHECK(copy && copy->version == 42);
    CHECK(copy->tables[0].rows[0].values[1].as.text != table->rows[0].values[1].as.text);
    CHECK(copy->tables[0].columns[1].default_value.as.text !=
          table->columns[1].default_value.as.text);
    adb_state_free(source);
    table = &copy->tables[0];
    CHECK(strcmp(table->rows[0].values[1].as.text, "Ada") == 0);
    CHECK(strcmp(table->columns[1].default_value.as.text, "Anonymous") == 0);
    ADBValue key = {.type = ADB_TEXT, .as.text = "Ada"};
    size_t count;
    const uint64_t *rows = adb_tree_find(table->indexes[0].tree, &key, &count);
    CHECK(rows && count == 1 && rows[0] == 1);

    /* Missing implicit PRIMARY/UNIQUE definitions must not bypass constraints. */
    table->rows[1].values[0] = integer(1);
    CHECK(!adb_table_reindex(table, &error) && error.code == ADB_ERR_CONSTRAINT);
    table->rows[1].values[0] = integer(2);
    table->rows[1].values[1] = (ADBValue){.type = ADB_TEXT,
                                        .as.text = adb_strdup("Ada", &error)};
    CHECK(table->rows[1].values[1].as.text);
    CHECK(!adb_table_reindex(table, &error) && error.code == ADB_ERR_CONSTRAINT);
    adb_value_free(&table->rows[1].values[1]);
    table->rows[1].values[0] = adb_null();
    CHECK(!adb_table_reindex(table, &error) && error.code == ADB_ERR_CONSTRAINT);
    table->rows[1].values[0] = integer(2);
    CHECK(adb_table_reindex(table, &error));

    table->rows[2].id = 2;
    CHECK(!adb_table_reindex(table, &error));
    table->rows[2].id = 3;
    table->next_row_id = 3;
    CHECK(!adb_table_reindex(table, &error));
    table->next_row_id = 4;
    CHECK(adb_table_reindex(table, &error));
    adb_state_free(copy);
}

int main(void)
{
    numeric_boundaries();
    stress_index(0);
    stress_index(1);
    stress_index(2);
    model_integrity();
    puts("index/model: 60,000 AVL keys, numeric boundaries, ownership and constraints OK");
    return EXIT_SUCCESS;
}
