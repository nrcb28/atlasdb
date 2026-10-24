#include "internal.h"

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The engine only touches private transaction/statement snapshots. Failed
 * statements can therefore be discarded without compensating mutations. */
typedef struct { const ADBTable *table; const ADBRow *row; } EvalContext;
typedef struct { const ADBExpr *expr; int column; char *name; } Projection;
typedef struct { const ADBRow *row; ADBValue order; } Candidate;

static bool is_numeric(const ADBValue *v) {
    return v->type == ADB_INTEGER || v->type == ADB_REAL;
}
static double as_real(const ADBValue *v) {
    return v->type == ADB_INTEGER ? (double)v->as.integer : v->as.real;
}
static ADBValue boolean_value(bool value) {
    ADBValue v = adb_null(); v.type = ADB_BOOLEAN; v.as.boolean = value; return v;
}
static ADBValue integer_value(int64_t value) {
    ADBValue v = adb_null(); v.type = ADB_INTEGER; v.as.integer = value; return v;
}
static bool aggregate(const ADBExpr *e) {
    return e && e->kind == EX_FUNCTION &&
        (adb_name_equal(e->name, "COUNT") || adb_name_equal(e->name, "SUM") ||
         adb_name_equal(e->name, "AVG") || adb_name_equal(e->name, "MIN") ||
         adb_name_equal(e->name, "MAX"));
}
static bool validate_expr(const ADBExpr *e, const ADBTable *table,
                          bool allow_aggregate, unsigned depth, ADBError *err) {
    if (!e) return adb_error(err, ADB_ERR_PARSE, "Missing expression");
    if (depth > ADB_MAX_EXPR_DEPTH) return adb_error(err, ADB_ERR_LIMIT, "Expression too deep");
    switch (e->kind) {
    case EX_LITERAL: return true;
    case EX_COLUMN:
        return table && adb_find_column(table, e->name) >= 0 ? true :
            adb_error(err, ADB_ERR_SCHEMA, "Unknown column '%s'", e->name);
    case EX_STAR: return adb_error(err, ADB_ERR_SCHEMA, "Wildcard is only valid as a projection or COUNT(*)");
    case EX_UNARY: return validate_expr(e->left, table, false, depth + 1, err);
    case EX_BINARY:
        return validate_expr(e->left, table, false, depth + 1, err) &&
               validate_expr(e->right, table, false, depth + 1, err);
    case EX_FUNCTION:
        if (aggregate(e)) {
            if (!allow_aggregate) return adb_error(err, ADB_ERR_SCHEMA, "Aggregate '%s' is not valid here", e->name);
            if (e->left && e->left->kind == EX_STAR && adb_name_equal(e->name, "COUNT")) return true;
            return validate_expr(e->left, table, false, depth + 1, err);
        }
        if (!adb_name_equal(e->name, "ABS") && !adb_name_equal(e->name, "LENGTH") &&
            !adb_name_equal(e->name, "LOWER") && !adb_name_equal(e->name, "UPPER"))
            return adb_error(err, ADB_ERR_SCHEMA, "Unknown function '%s'", e->name);
        return validate_expr(e->left, table, false, depth + 1, err);
    }
    return adb_error(err, ADB_ERR_PARSE, "Invalid expression");
}

static bool like_match(const char *text, const char *pattern, bool *matches, ADBError *err) {
    const char *star = NULL, *retry = NULL;
    size_t steps = 0;
    while (*text) {
        if (++steps > 10000000u) return adb_error(err, ADB_ERR_LIMIT, "LIKE pattern requires too much work");
        if (*pattern == '%') { star = ++pattern; retry = text; }
        else if (*pattern == '_' || (*pattern && *pattern == *text)) { pattern++; text++; }
        else if (star) { pattern = star; text = ++retry; }
        else { *matches = false; return true; }
    }
    while (*pattern == '%') pattern++;
    *matches = !*pattern;
    return true;
}
static bool checked_arithmetic(ADBOp op, const ADBValue *a, const ADBValue *b,
                               ADBValue *out, ADBError *err) {
    if (!is_numeric(a) || !is_numeric(b)) return adb_error(err, ADB_ERR_TYPE, "Arithmetic requires numeric operands");
    if ((op == OP_DIV || op == OP_MOD) && as_real(b) == 0.0)
        return adb_error(err, ADB_ERR_TYPE, "Division by zero");
    if (op == OP_MOD && (a->type != ADB_INTEGER || b->type != ADB_INTEGER))
        return adb_error(err, ADB_ERR_TYPE, "Modulo requires integers");
    if (a->type == ADB_INTEGER && b->type == ADB_INTEGER) {
        int64_t x = a->as.integer, y = b->as.integer, z = 0;
        bool overflow = false;
        switch (op) {
        case OP_ADD: overflow = __builtin_add_overflow(x, y, &z); break;
        case OP_SUB: overflow = __builtin_sub_overflow(x, y, &z); break;
        case OP_MUL: overflow = __builtin_mul_overflow(x, y, &z); break;
        case OP_DIV:
            if (x == INT64_MIN && y == -1) overflow = true; else z = x / y;
            break;
        case OP_MOD: z = (x == INT64_MIN && y == -1) ? 0 : x % y; break;
        default: return adb_error(err, ADB_ERR_TYPE, "Invalid arithmetic operator");
        }
        if (overflow) return adb_error(err, ADB_ERR_TYPE, "Integer arithmetic overflow");
        *out = integer_value(z); return true;
    }
    double x = as_real(a), y = as_real(b), z;
    switch (op) {
    case OP_ADD: z = x + y; break;
    case OP_SUB: z = x - y; break;
    case OP_MUL: z = x * y; break;
    case OP_DIV: z = x / y; break;
    default: return adb_error(err, ADB_ERR_TYPE, "Invalid arithmetic operator");
    }
    if (!isfinite(z)) return adb_error(err, ADB_ERR_TYPE, "Real arithmetic overflow");
    out->type = ADB_REAL; out->as.real = z; return true;
}
static bool eval(const ADBExpr *e, const EvalContext *ctx, ADBValue *out, ADBError *err);
static bool eval_function(const ADBExpr *e, const EvalContext *ctx, ADBValue *out, ADBError *err) {
    ADBValue arg = adb_null();
    if (!eval(e->left, ctx, &arg, err)) return false;
    bool ok = true;
    if (arg.type == ADB_NULL) { *out = adb_null(); return true; }
    if (adb_name_equal(e->name, "ABS")) {
        if (!is_numeric(&arg)) ok = adb_error(err, ADB_ERR_TYPE, "ABS requires a numeric argument");
        else if (arg.type == ADB_INTEGER) {
            if (arg.as.integer == INT64_MIN) ok = adb_error(err, ADB_ERR_TYPE, "Integer ABS overflow");
            else { *out = arg; if (out->as.integer < 0) out->as.integer = -out->as.integer; }
        } else { *out = arg; out->as.real = fabs(arg.as.real); }
    } else if (adb_name_equal(e->name, "LENGTH")) {
        if (arg.type != ADB_TEXT) ok = adb_error(err, ADB_ERR_TYPE, "LENGTH requires TEXT");
        else *out = integer_value((int64_t)strlen(arg.as.text));
    } else if (adb_name_equal(e->name, "LOWER") || adb_name_equal(e->name, "UPPER")) {
        if (arg.type != ADB_TEXT) ok = adb_error(err, ADB_ERR_TYPE, "%s requires TEXT", e->name);
        else {
            *out = arg; arg = adb_null();
            for (char *p = out->as.text; *p; p++)
                *p = (char)(adb_name_equal(e->name, "LOWER") ? tolower((unsigned char)*p) : toupper((unsigned char)*p));
        }
    } else ok = adb_error(err, ADB_ERR_SCHEMA, "Function '%s' is not a scalar function", e->name);
    adb_value_free(&arg); return ok;
}
static bool eval(const ADBExpr *e, const EvalContext *ctx, ADBValue *out, ADBError *err) {
    *out = adb_null();
    if (!e) return adb_error(err, ADB_ERR_PARSE, "Missing expression");
    if (e->kind == EX_LITERAL) return adb_value_copy(out, &e->literal, err);
    if (e->kind == EX_COLUMN) {
        int col = ctx && ctx->table ? adb_find_column(ctx->table, e->name) : -1;
        if (col < 0 || !ctx->row) return adb_error(err, ADB_ERR_SCHEMA, "Unknown column '%s'", e->name);
        return adb_value_copy(out, &ctx->row->values[col], err);
    }
    if (e->kind == EX_FUNCTION) return eval_function(e, ctx, out, err);
    if (e->kind != EX_UNARY && e->kind != EX_BINARY)
        return adb_error(err, ADB_ERR_SCHEMA, "Wildcard cannot be evaluated here");
    ADBValue a = adb_null(), b = adb_null();
    if (!eval(e->left, ctx, &a, err)) return false;
    bool ok = true;
    if (e->kind == EX_UNARY) {
        switch (e->op) {
        case OP_IS_NULL: *out = boolean_value(a.type == ADB_NULL); break;
        case OP_IS_NOT_NULL: *out = boolean_value(a.type != ADB_NULL); break;
        case OP_NOT:
            if (a.type != ADB_NULL) *out = boolean_value(!adb_value_truth(&a));
            break;
        case OP_NEG:
            if (a.type == ADB_NULL) break;
            if (!is_numeric(&a)) ok = adb_error(err, ADB_ERR_TYPE, "Unary minus requires a number");
            else if (a.type == ADB_INTEGER && a.as.integer == INT64_MIN)
                ok = adb_error(err, ADB_ERR_TYPE, "Integer negation overflow");
            else { *out = a; if (a.type == ADB_INTEGER) out->as.integer = -a.as.integer; else out->as.real = -a.as.real; }
            break;
        default: ok = adb_error(err, ADB_ERR_PARSE, "Invalid unary operator"); break;
        }
        adb_value_free(&a); return ok;
    }
    /* SQL three-valued logic, with short-circuiting on decisive values. */
    if (e->op == OP_AND && a.type != ADB_NULL && !adb_value_truth(&a)) {
        *out = boolean_value(false); adb_value_free(&a); return true;
    }
    if (e->op == OP_OR && a.type != ADB_NULL && adb_value_truth(&a)) {
        *out = boolean_value(true); adb_value_free(&a); return true;
    }
    if (!eval(e->right, ctx, &b, err)) { adb_value_free(&a); return false; }
    if (e->op == OP_AND || e->op == OP_OR) {
        bool av = adb_value_truth(&a), bv = adb_value_truth(&b);
        if (e->op == OP_AND && ((a.type != ADB_NULL && !av) || (b.type != ADB_NULL && !bv))) *out = boolean_value(false);
        else if (e->op == OP_OR && ((a.type != ADB_NULL && av) || (b.type != ADB_NULL && bv))) *out = boolean_value(true);
        else if (a.type != ADB_NULL && b.type != ADB_NULL) *out = boolean_value(e->op == OP_AND ? av && bv : av || bv);
    } else if (a.type == ADB_NULL || b.type == ADB_NULL) {
        /* All remaining binary operators propagate NULL. */
    } else if (e->op >= OP_ADD && e->op <= OP_MOD) ok = checked_arithmetic(e->op, &a, &b, out, err);
    else if (e->op >= OP_EQ && e->op <= OP_GE) {
        if (a.type != b.type && !(is_numeric(&a) && is_numeric(&b)))
            ok = adb_error(err, ADB_ERR_TYPE, "Cannot compare %s with %s", adb_type_name(a.type), adb_type_name(b.type));
        else {
            int c = adb_value_compare(&a, &b); bool value = false;
            switch (e->op) {
            case OP_EQ: value = c == 0; break; case OP_NE: value = c != 0; break;
            case OP_LT: value = c < 0; break; case OP_LE: value = c <= 0; break;
            case OP_GT: value = c > 0; break; case OP_GE: value = c >= 0; break;
            default: break;
            }
            *out = boolean_value(value);
        }
    } else if (e->op == OP_LIKE || e->op == OP_NOT_LIKE) {
        bool match = false;
        if (a.type != ADB_TEXT || b.type != ADB_TEXT) ok = adb_error(err, ADB_ERR_TYPE, "LIKE requires TEXT operands");
        else if ((ok = like_match(a.as.text, b.as.text, &match, err))) *out = boolean_value(e->op == OP_LIKE ? match : !match);
    } else ok = adb_error(err, ADB_ERR_PARSE, "Invalid binary operator");
    adb_value_free(&a); adb_value_free(&b); return ok;
}
static bool matches(const ADBExpr *where, const ADBTable *table, const ADBRow *row, bool *yes, ADBError *err) {
    if (!where) { *yes = true; return true; }
    ADBValue v = adb_null(); EvalContext ctx = {table, row};
    if (!eval(where, &ctx, &v, err)) return false;
    *yes = v.type != ADB_NULL && adb_value_truth(&v); adb_value_free(&v); return true;
}

static bool message(ADBResult *r, ADBError *err, const char *text) {
    return adb_result_message(r, "%s", text) ? true : adb_error(err, ADB_ERR_MEMORY, "Out of memory creating result");
}
static bool index_name_exists(const ADBState *state, const ADBTable *pending, const char *name) {
    for (size_t i = 0; i < state->ntables; i++)
        for (size_t j = 0; j < state->tables[i].nindexes; j++)
            if (adb_name_equal(state->tables[i].indexes[j].name, name)) return true;
    if (pending) for (size_t i = 0; i < pending->nindexes; i++)
        if (adb_name_equal(pending->indexes[i].name, name)) return true;
    return false;
}
static char *implicit_index_name(const ADBState *state, const ADBTable *table,
                                 const ADBColumn *column, ADBError *err) {
    size_t length = strlen(table->name) + strlen(column->name) + 48;
    char *name = adb_alloc(length, 1, err);
    if (!name) return NULL;
    snprintf(name, length, "__%s_%s_%s", table->name, column->name, column->primary_key ? "pk" : "uq");
    uint64_t suffix = 0;
    while (index_name_exists(state, table, name))
        snprintf(name, length, "__%s_%s_%s_%" PRIu64, table->name, column->name,
                 column->primary_key ? "pk" : "uq", ++suffix);
    return name;
}
static bool create_table(ADBState *state, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    if (adb_find_table(state, s->table)) {
        if (s->if_not_exists) return message(r, err, "Table already exists");
        return adb_error(err, ADB_ERR_SCHEMA, "Table '%s' already exists", s->table);
    }
    if (state->ntables >= ADB_MAX_TABLES) return adb_error(err, ADB_ERR_LIMIT, "Too many tables");
    if (!s->ncolumns || s->ncolumns > ADB_MAX_COLUMNS) return adb_error(err, ADB_ERR_LIMIT, "Invalid column count");
    unsigned primaries = 0;
    for (size_t i = 0; i < s->ncolumns; i++) {
        primaries += s->columns[i].primary_key;
        for (size_t j = 0; j < i; j++)
            if (adb_name_equal(s->columns[i].name, s->columns[j].name)) return adb_error(err, ADB_ERR_SCHEMA, "Duplicate column '%s'", s->columns[i].name);
    }
    if (primaries > 1) return adb_error(err, ADB_ERR_SCHEMA, "Only one single-column PRIMARY KEY is supported");
    ADBTable table = {0}; table.next_row_id = 1;
    table.name = adb_strdup(s->table, err);
    table.columns = adb_alloc(s->ncolumns, sizeof(*table.columns), err);
    if (!table.name || !table.columns) { adb_table_free(&table); return false; }
    table.ncolumns = s->ncolumns;
    for (size_t i = 0; i < table.ncolumns; i++) {
        ADBColumn *col = &table.columns[i];
        if (!adb_column_copy(col, &s->columns[i], err)) { adb_table_free(&table); return false; }
        if (col->primary_key) { col->not_null = true; col->unique = true; }
        if (col->has_default) {
            if (!adb_value_coerce(&col->default_value, col->type, err)) { adb_table_free(&table); return false; }
            if (col->not_null && col->default_value.type == ADB_NULL) {
                adb_table_free(&table); return adb_error(err, ADB_ERR_CONSTRAINT, "NOT NULL column cannot have a NULL default");
            }
        }
        if (col->primary_key || col->unique) {
            ADBIndex *grown = realloc(table.indexes, (table.nindexes + 1) * sizeof(*grown));
            if (!grown) { adb_table_free(&table); return adb_error(err, ADB_ERR_MEMORY, "Out of memory creating index"); }
            table.indexes = grown; ADBIndex *index = &table.indexes[table.nindexes++]; memset(index, 0, sizeof(*index));
            index->name = implicit_index_name(state, &table, col, err);
            if (!index->name) { adb_table_free(&table); return false; }
            index->column = i; index->unique = true; index->implicit = true;
        }
    }
    if (!adb_table_reindex(&table, err)) { adb_table_free(&table); return false; }
    ADBTable *grown = realloc(state->tables, (state->ntables + 1) * sizeof(*grown));
    if (!grown) { adb_table_free(&table); return adb_error(err, ADB_ERR_MEMORY, "Out of memory creating table"); }
    state->tables = grown; state->tables[state->ntables++] = table; *changed = true;
    return message(r, err, "Table created");
}
static bool drop_table(ADBState *state, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    for (size_t i = 0; i < state->ntables; i++) if (adb_name_equal(state->tables[i].name, s->table)) {
        adb_table_free(&state->tables[i]);
        memmove(&state->tables[i], &state->tables[i + 1], (state->ntables - i - 1) * sizeof(*state->tables));
        state->ntables--; *changed = true; return message(r, err, "Table dropped");
    }
    if (s->if_exists) return message(r, err, "Table does not exist");
    return adb_error(err, ADB_ERR_SCHEMA, "Unknown table '%s'", s->table);
}
static bool insert_rows(ADBTable *t, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    size_t width = s->ninsert_columns ? s->ninsert_columns : t->ncolumns;
    size_t *columns = adb_alloc(width, sizeof(*columns), err);
    if (!columns) return false;
    for (size_t i = 0; i < width; i++) {
        int c = s->ninsert_columns ? adb_find_column(t, s->insert_columns[i]) : (int)i;
        if (c < 0) { free(columns); return adb_error(err, ADB_ERR_SCHEMA, "Unknown column '%s'", s->insert_columns[i]); }
        columns[i] = (size_t)c;
        for (size_t j = 0; j < i; j++) if (columns[j] == columns[i]) {
            free(columns); return adb_error(err, ADB_ERR_SCHEMA, "Duplicate INSERT column");
        }
    }
    if (s->nvalue_rows > ADB_MAX_ROWS - t->nrows) { free(columns); return adb_error(err, ADB_ERR_LIMIT, "Table row limit exceeded"); }
    for (size_t i = 0; i < s->nvalue_rows; i++) {
        if (s->value_counts[i] != width) { free(columns); return adb_error(err, ADB_ERR_SCHEMA, "INSERT row %zu has %zu values; expected %zu", i + 1, s->value_counts[i], width); }
        for (size_t j = 0; j < width; j++) if (!validate_expr(s->values[i][j], NULL, false, 0, err)) { free(columns); return false; }
    }
    size_t needed = t->nrows + s->nvalue_rows;
    if (needed > t->row_capacity) {
        size_t cap = t->row_capacity ? t->row_capacity : 16;
        while (cap < needed) cap = cap > ADB_MAX_ROWS / 2 ? ADB_MAX_ROWS : cap * 2;
        ADBRow *grown = realloc(t->rows, cap * sizeof(*grown));
        if (!grown) { free(columns); return adb_error(err, ADB_ERR_MEMORY, "Out of memory adding rows"); }
        t->rows = grown; t->row_capacity = cap;
    }
    for (size_t i = 0; i < s->nvalue_rows; i++) {
        if (t->next_row_id == UINT64_MAX) { free(columns); return adb_error(err, ADB_ERR_LIMIT, "Row ID exhausted"); }
        ADBRow *row = &t->rows[t->nrows]; memset(row, 0, sizeof(*row));
        row->values = adb_alloc(t->ncolumns, sizeof(*row->values), err);
        if (!row->values) { free(columns); return false; }
        row->id = t->next_row_id++; t->nrows++;
        for (size_t j = 0; j < t->ncolumns; j++) if (t->columns[j].has_default &&
            !adb_value_copy(&row->values[j], &t->columns[j].default_value, err)) { free(columns); return false; }
        for (size_t j = 0; j < width; j++) {
            ADBValue v = adb_null();
            if (!eval(s->values[i][j], NULL, &v, err) || !adb_value_coerce(&v, t->columns[columns[j]].type, err)) {
                adb_value_free(&v); free(columns); return false;
            }
            adb_value_free(&row->values[columns[j]]); row->values[columns[j]] = v;
        }
    }
    free(columns);
    if (!adb_table_reindex(t, err)) return false;
    r->affected = s->nvalue_rows; *changed = s->nvalue_rows > 0;
    return message(r, err, "Rows inserted");
}

static const ADBIndex *find_index(const ADBTable *t, const ADBExpr *where, const ADBValue **key) {
    if (!where) return NULL;
    if (where->kind == EX_BINARY && where->op == OP_AND) {
        const ADBIndex *index = find_index(t, where->left, key);
        return index ? index : find_index(t, where->right, key);
    }
    if (where->kind != EX_BINARY || where->op != OP_EQ) return NULL;
    const ADBExpr *col = where->left, *literal = where->right;
    if (col->kind != EX_COLUMN || literal->kind != EX_LITERAL) { col = where->right; literal = where->left; }
    if (col->kind != EX_COLUMN || literal->kind != EX_LITERAL) return NULL;
    int column = adb_find_column(t, col->name);
    if (column < 0) return NULL;
    ADBType type = t->columns[column].type;
    if (literal->literal.type != ADB_NULL && literal->literal.type != type &&
        !((type == ADB_REAL || type == ADB_INTEGER) && is_numeric(&literal->literal))) return NULL;
    for (size_t i = 0; i < t->nindexes; i++) if (t->indexes[i].column == (size_t)column && t->indexes[i].tree) {
        *key = &literal->literal; return &t->indexes[i];
    }
    return NULL;
}
static const ADBRow *find_row_id(const ADBTable *t, uint64_t id) {
    size_t lo = 0, hi = t->nrows;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->rows[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return lo < t->nrows && t->rows[lo].id == id ? &t->rows[lo] : NULL;
}
static bool candidates(const ADBTable *t, const ADBStatement *s, Candidate **out, size_t *count, ADBError *err) {
    const ADBValue *key = NULL;
    const ADBIndex *index = find_index(t, s->where, &key);
    const uint64_t *ids = NULL;
    size_t n = t->nrows;
    if (index) {
        n = 0;
        if (key->type != ADB_NULL) ids = adb_tree_find(index->tree, key, &n);
    }
    Candidate *list = adb_alloc(n ? n : 1, sizeof(*list), err);
    if (!list) return false;
    *count = 0;
    for (size_t i = 0; i < n; i++) {
        const ADBRow *row = index ? find_row_id(t, ids[i]) : &t->rows[i];
        bool yes;
        if (!row) { free(list); return adb_error(err, ADB_ERR_CORRUPT, "Index points to missing row"); }
        if (!matches(s->where, t, row, &yes, err)) { free(list); return false; }
        if (yes) list[(*count)++].row = row;
    }
    *out = list; return true;
}
static void free_candidates(Candidate *list, size_t count) {
    if (!list) return;
    for (size_t i = 0; i < count; i++) adb_value_free(&list[i].order);
    free(list);
}
static void projections_free(Projection *p, size_t count) {
    if (!p) return;
    for (size_t i = 0; i < count; i++) free(p[i].name);
    free(p);
}
static bool prepare_projections(const ADBTable *t, const ADBStatement *s, Projection **out,
                               size_t *count, bool *agg, ADBError *err) {
    *count = 0; *agg = false;
    size_t n = 0;
    for (size_t i = 0; i < s->nprojections; i++) {
        size_t add = s->projections[i].expression->kind == EX_STAR ? t->ncolumns : 1;
        if (add > ADB_MAX_COLUMNS - n) return adb_error(err, ADB_ERR_LIMIT, "Too many projected columns");
        n += add;
    }
    if (!n) return adb_error(err, ADB_ERR_SCHEMA, "SELECT needs a projection");
    Projection *p = adb_alloc(n, sizeof(*p), err);
    if (!p) return false;
    bool nonagg = false;
    for (size_t i = 0; i < s->nprojections; i++) {
        const ADBProjection *item = &s->projections[i];
        if (item->expression->kind == EX_STAR) {
            if (item->alias) { projections_free(p, n); return adb_error(err, ADB_ERR_SCHEMA, "Wildcard cannot have an alias"); }
            nonagg = true;
            for (size_t j = 0; j < t->ncolumns; j++) {
                Projection *one = &p[(*count)++]; one->column = (int)j;
                one->name = adb_strdup(t->columns[j].name, err);
                if (!one->name) { projections_free(p, n); return false; }
            }
        } else {
            if (!validate_expr(item->expression, t, true, 0, err)) { projections_free(p, n); return false; }
            bool isagg = aggregate(item->expression); *agg |= isagg; nonagg |= !isagg;
            Projection *one = &p[(*count)++]; one->expr = item->expression; one->column = -1;
            char generated[64]; snprintf(generated, sizeof(generated), "expression_%zu", *count);
            const char *name = item->alias ? item->alias :
                (item->expression->kind == EX_COLUMN || item->expression->kind == EX_FUNCTION) ? item->expression->name : generated;
            one->name = adb_strdup(name, err);
            if (!one->name) { projections_free(p, n); return false; }
        }
    }
    if (*agg && nonagg) { projections_free(p, n); return adb_error(err, ADB_ERR_SCHEMA, "Cannot mix aggregate and nonaggregate projections without GROUP BY"); }
    *out = p; return true;
}
static bool evaluate_projection(const Projection *p, const ADBTable *t, const ADBRow *row, ADBValue *value, ADBError *err) {
    if (p->column >= 0) return adb_value_copy(value, &row->values[p->column], err);
    EvalContext ctx = {t, row}; return eval(p->expr, &ctx, value, err);
}
static void sort_merge(Candidate *list, Candidate *temp, size_t lo, size_t hi, bool desc) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    sort_merge(list, temp, lo, mid, desc); sort_merge(list, temp, mid, hi, desc);
    size_t a = lo, b = mid, k = lo;
    while (a < mid && b < hi) {
        int comparison = adb_value_compare(&list[a].order, &list[b].order);
        if (desc) comparison = -comparison;
        temp[k++] = comparison <= 0 ? list[a++] : list[b++];
    }
    while (a < mid) temp[k++] = list[a++];
    while (b < hi) temp[k++] = list[b++];
    memcpy(list + lo, temp + lo, (hi - lo) * sizeof(*list));
}
static bool order_candidates(Candidate *list, size_t count, const ADBTable *t, const ADBStatement *s,
                             const Projection *p, size_t nprojections, bool isagg, ADBError *err) {
    if (!s->order_column) return true;
    int projection = -1;
    for (size_t i = 0; i < nprojections; i++) if (adb_name_equal(p[i].name, s->order_column)) { projection = (int)i; break; }
    int column = adb_find_column(t, s->order_column);
    if (projection < 0 && (column < 0 || isagg)) return adb_error(err, ADB_ERR_SCHEMA, "Unknown ORDER BY column '%s'", s->order_column);
    if (isagg) return true;
    for (size_t i = 0; i < count; i++) {
        if (projection >= 0) {
            if (!evaluate_projection(&p[projection], t, list[i].row, &list[i].order, err)) return false;
        } else if (!adb_value_copy(&list[i].order, &list[i].row->values[column], err)) return false;
    }
    Candidate *temp = adb_alloc(count ? count : 1, sizeof(*temp), err);
    if (!temp) return false;
    sort_merge(list, temp, 0, count, s->order_desc); free(temp); return true;
}
static bool aggregate_value(const ADBExpr *e, const ADBTable *t, const Candidate *list,
                            size_t count, ADBValue *out, ADBError *err) {
    bool iscount = adb_name_equal(e->name, "COUNT");
    bool isavg = adb_name_equal(e->name, "AVG"), issum = adb_name_equal(e->name, "SUM");
    bool ismin = adb_name_equal(e->name, "MIN");
    uint64_t seen = 0; long double mean = 0.0L;
    *out = adb_null();
    for (size_t i = 0; i < count; i++) {
        ADBValue v = adb_null();
        if (iscount && e->left->kind == EX_STAR) v = integer_value(1);
        else {
            EvalContext ctx = {t, list[i].row};
            if (!eval(e->left, &ctx, &v, err)) { adb_value_free(out); return false; }
        }
        if (v.type == ADB_NULL) continue;
        seen++;
        if ((isavg || issum) && !is_numeric(&v)) {
            adb_value_free(&v); adb_value_free(out); return adb_error(err, ADB_ERR_TYPE, "%s requires numeric values", e->name);
        }
        if (isavg) {
            long double x = v.type == ADB_INTEGER ? (long double)v.as.integer : (long double)v.as.real;
            mean += (x - mean) / (long double)seen;
        } else if (issum) {
            if (out->type == ADB_NULL) { *out = v; v = adb_null(); }
            else {
                ADBValue total = adb_null();
                if (!checked_arithmetic(OP_ADD, out, &v, &total, err)) { adb_value_free(&v); adb_value_free(out); return false; }
                adb_value_free(out); *out = total;
            }
        } else if (!iscount) {
            if (out->type != ADB_NULL && out->type != v.type && !(is_numeric(out) && is_numeric(&v))) {
                adb_value_free(&v); adb_value_free(out); return adb_error(err, ADB_ERR_TYPE, "Incomparable aggregate values");
            }
            int c = out->type == ADB_NULL ? 0 : adb_value_compare(&v, out);
            if (out->type == ADB_NULL || (ismin ? c < 0 : c > 0)) { adb_value_free(out); *out = v; v = adb_null(); }
        }
        adb_value_free(&v);
    }
    if (iscount) *out = integer_value((int64_t)seen);
    else if (isavg && seen) {
        double v = (double)mean;
        if (!isfinite(v)) return adb_error(err, ADB_ERR_TYPE, "AVG overflow");
        out->type = ADB_REAL; out->as.real = v;
    }
    return true;
}
static bool select_rows(const ADBTable *t, const ADBStatement *s, ADBResult *r, ADBError *err) {
    if (s->where && !validate_expr(s->where, t, false, 0, err)) return false;
    Projection *p = NULL; size_t nprojections = 0; bool isagg;
    if (!prepare_projections(t, s, &p, &nprojections, &isagg, err)) return false;
    Candidate *list = NULL; size_t count = 0; bool ok = false;
    ADBValue *values = NULL;
    if (!candidates(t, s, &list, &count, err)) goto done;
    if (!order_candidates(list, count, t, s, p, nprojections, isagg, err)) goto done;
    const char **names = adb_alloc(nprojections, sizeof(*names), err);
    if (!names) goto done;
    for (size_t i = 0; i < nprojections; i++) names[i] = p[i].name;
    bool cols_ok = adb_result_columns(r, nprojections, names, err); free(names);
    if (!cols_ok) goto done;
    values = adb_alloc(nprojections, sizeof(*values), err);
    if (!values) goto done;
    if (isagg) {
        if (s->offset == 0 && (!s->has_limit || s->limit > 0)) {
            for (size_t i = 0; i < nprojections; i++) if (!aggregate_value(p[i].expr, t, list, count, &values[i], err)) goto done;
            if (!adb_result_row(r, values, err)) goto done;
        }
    } else {
        uint64_t end = count;
        if (s->has_limit && s->limit < end - (s->offset < end ? s->offset : end)) end = s->offset + s->limit;
        for (uint64_t row = s->offset; row < end; row++) {
            for (size_t i = 0; i < nprojections; i++) if (!evaluate_projection(&p[i], t, list[row].row, &values[i], err)) goto done;
            if (!adb_result_row(r, values, err)) goto done;
            for (size_t i = 0; i < nprojections; i++) adb_value_free(&values[i]);
        }
    }
    ok = true;
done:
    if (values) { for (size_t i = 0; i < nprojections; i++) adb_value_free(&values[i]); free(values); }
    free_candidates(list, count); projections_free(p, nprojections); return ok;
}

static bool update_rows(ADBTable *t, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    if (!s->nassignments) return adb_error(err, ADB_ERR_SCHEMA, "UPDATE needs assignments");
    if (s->where && !validate_expr(s->where, t, false, 0, err)) return false;
    size_t *columns = adb_alloc(s->nassignments, sizeof(*columns), err);
    if (!columns) return false;
    for (size_t i = 0; i < s->nassignments; i++) {
        int c = adb_find_column(t, s->assignments[i].column);
        if (c < 0) { free(columns); return adb_error(err, ADB_ERR_SCHEMA, "Unknown column '%s'", s->assignments[i].column); }
        columns[i] = (size_t)c;
        for (size_t j = 0; j < i; j++) if (columns[j] == columns[i]) {
            free(columns); return adb_error(err, ADB_ERR_SCHEMA, "Duplicate UPDATE assignment");
        }
        if (!validate_expr(s->assignments[i].value, t, false, 0, err)) { free(columns); return false; }
    }
    uint64_t count = 0;
    for (size_t i = 0; i < t->nrows; i++) {
        bool yes;
        if (!matches(s->where, t, &t->rows[i], &yes, err)) { free(columns); return false; }
        if (!yes) continue;
        ADBValue *values = adb_alloc(s->nassignments, sizeof(*values), err);
        if (!values) { free(columns); return false; }
        EvalContext ctx = {t, &t->rows[i]}; bool ok = true;
        for (size_t j = 0; j < s->nassignments; j++) {
            if (!eval(s->assignments[j].value, &ctx, &values[j], err) ||
                !adb_value_coerce(&values[j], t->columns[columns[j]].type, err)) { ok = false; break; }
        }
        if (ok) for (size_t j = 0; j < s->nassignments; j++) {
            adb_value_free(&t->rows[i].values[columns[j]]);
            t->rows[i].values[columns[j]] = values[j]; values[j] = adb_null();
        }
        for (size_t j = 0; j < s->nassignments; j++) adb_value_free(&values[j]);
        free(values);
        if (!ok) { free(columns); return false; }
        count++;
    }
    free(columns);
    if (count && !adb_table_reindex(t, err)) return false;
    r->affected = count; *changed = count > 0; return message(r, err, "Rows updated");
}
static bool delete_rows(ADBTable *t, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    if (s->where && !validate_expr(s->where, t, false, 0, err)) return false;
    bool *remove = adb_alloc(t->nrows ? t->nrows : 1, sizeof(*remove), err);
    if (!remove) return false;
    size_t count = 0;
    for (size_t i = 0; i < t->nrows; i++) {
        if (!matches(s->where, t, &t->rows[i], &remove[i], err)) { free(remove); return false; }
        count += remove[i];
    }
    size_t write = 0;
    for (size_t i = 0; i < t->nrows; i++) {
        if (remove[i]) {
            for (size_t j = 0; j < t->ncolumns; j++) adb_value_free(&t->rows[i].values[j]);
            free(t->rows[i].values);
        } else { if (write != i) t->rows[write] = t->rows[i]; write++; }
    }
    free(remove); t->nrows = write;
    if (count && !adb_table_reindex(t, err)) return false;
    r->affected = count; *changed = count > 0; return message(r, err, "Rows deleted");
}
static bool create_index(ADBState *state, ADBTable *t, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    if (index_name_exists(state, NULL, s->index_name)) {
        if (s->if_not_exists) return message(r, err, "Index already exists");
        return adb_error(err, ADB_ERR_SCHEMA, "Index '%s' already exists", s->index_name);
    }
    int col = adb_find_column(t, s->order_column);
    if (col < 0) return adb_error(err, ADB_ERR_SCHEMA, "Unknown index column '%s'", s->order_column);
    if (t->nindexes >= 4096u) return adb_error(err, ADB_ERR_LIMIT, "Too many indexes on table");
    ADBIndex *grown = realloc(t->indexes, (t->nindexes + 1) * sizeof(*grown));
    if (!grown) return adb_error(err, ADB_ERR_MEMORY, "Out of memory creating index");
    t->indexes = grown; ADBIndex *idx = &t->indexes[t->nindexes++]; memset(idx, 0, sizeof(*idx));
    idx->name = adb_strdup(s->index_name, err); idx->column = (size_t)col; idx->unique = s->unique;
    if (!idx->name || !adb_table_reindex(t, err)) return false;
    *changed = true; return message(r, err, "Index created");
}
static bool drop_index(ADBState *state, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    for (size_t i = 0; i < state->ntables; i++) {
        ADBTable *t = &state->tables[i];
        for (size_t j = 0; j < t->nindexes; j++) if (adb_name_equal(t->indexes[j].name, s->index_name)) {
            ADBIndex *idx = &t->indexes[j];
            if (idx->implicit) return adb_error(err, ADB_ERR_CONSTRAINT, "Cannot drop an index required by a column constraint");
            free(idx->name); adb_tree_free(idx->tree);
            memmove(idx, idx + 1, (t->nindexes - j - 1) * sizeof(*idx)); t->nindexes--;
            *changed = true; return message(r, err, "Index dropped");
        }
    }
    if (s->if_exists) return message(r, err, "Index does not exist");
    return adb_error(err, ADB_ERR_SCHEMA, "Unknown index '%s'", s->index_name);
}
static bool show_tables(const ADBState *state, ADBResult *r, ADBError *err) {
    const char *names[] = {"table", "rows", "columns", "indexes"};
    if (!adb_result_columns(r, 4, names, err)) return false;
    for (size_t i = 0; i < state->ntables; i++) {
        const ADBTable *t = &state->tables[i];
        ADBValue row[4] = {adb_null(), integer_value((int64_t)t->nrows), integer_value((int64_t)t->ncolumns), integer_value((int64_t)t->nindexes)};
        row[0].type = ADB_TEXT; row[0].as.text = t->name;
        if (!adb_result_row(r, row, err)) return false;
    }
    return true;
}
static bool describe(const ADBTable *t, ADBResult *r, ADBError *err) {
    const char *names[] = {"column", "type", "nullable", "primary_key", "unique", "default"};
    if (!adb_result_columns(r, 6, names, err)) return false;
    for (size_t i = 0; i < t->ncolumns; i++) {
        const ADBColumn *c = &t->columns[i];
        ADBValue row[6] = {adb_null(), adb_null(), boolean_value(!c->not_null && !c->primary_key),
            boolean_value(c->primary_key), boolean_value(c->unique || c->primary_key), adb_null()};
        row[0].type = row[1].type = ADB_TEXT; row[0].as.text = c->name;
        row[1].as.text = (char *)adb_type_name(c->type);
        if (c->has_default) row[5] = c->default_value;
        if (!adb_result_row(r, row, err)) return false;
    }
    return true;
}
static bool explain(ADBState *state, const ADBStatement *outer, ADBResult *r, ADBError *err) {
    const ADBStatement *s = outer->inner;
    if (!s || s->kind != ST_SELECT) return adb_error(err, ADB_ERR_SCHEMA, "EXPLAIN supports SELECT only");
    ADBTable *t = adb_find_table(state, s->table);
    if (!t) return adb_error(err, ADB_ERR_SCHEMA, "Unknown table '%s'", s->table);
    if (s->where && !validate_expr(s->where, t, false, 0, err)) return false;
    Projection *p = NULL; size_t n = 0; bool isagg;
    if (!prepare_projections(t, s, &p, &n, &isagg, err)) return false;
    bool valid_order = order_candidates(NULL, 0, t, s, p, n, isagg, err);
    projections_free(p, n);
    if (!valid_order) return false;
    const ADBValue *key = NULL; const ADBIndex *idx = find_index(t, s->where, &key);
    size_t estimated = t->nrows;
    if (idx) { estimated = 0; if (key->type != ADB_NULL) (void)adb_tree_find(idx->tree, key, &estimated); }
    const char *names[] = {"operation", "table", "index", "estimated_rows"};
    if (!adb_result_columns(r, 4, names, err)) return false;
    ADBValue row[4] = {adb_null(), adb_null(), adb_null(), integer_value((int64_t)estimated)};
    row[0].type = row[1].type = ADB_TEXT;
    row[0].as.text = idx ? "INDEX LOOKUP" : "TABLE SCAN"; row[1].as.text = t->name;
    if (idx) { row[2].type = ADB_TEXT; row[2].as.text = idx->name; }
    return adb_result_row(r, row, err);
}

bool adb_statement_writes(const ADBStatement *s) {
    switch (s->kind) {
    case ST_CREATE_TABLE: case ST_DROP_TABLE: case ST_INSERT: case ST_UPDATE:
    case ST_DELETE: case ST_CREATE_INDEX: case ST_DROP_INDEX: return true;
    default: return false;
    }
}
bool adb_engine_execute(ADBState *state, const ADBStatement *s, ADBResult *r, bool *changed, ADBError *err) {
    *changed = false;
    if (!state || !s || !r) return adb_error(err, ADB_ERR_SCHEMA, "Invalid execution arguments");
    switch (s->kind) {
    case ST_CREATE_TABLE: return create_table(state, s, r, changed, err);
    case ST_DROP_TABLE: return drop_table(state, s, r, changed, err);
    case ST_DROP_INDEX: return drop_index(state, s, r, changed, err);
    case ST_SHOW_TABLES: return show_tables(state, r, err);
    case ST_EXPLAIN: return explain(state, s, r, err);
    default: break;
    }
    if (s->kind == ST_BEGIN || s->kind == ST_COMMIT || s->kind == ST_ROLLBACK || s->kind == ST_CHECKPOINT)
        return adb_error(err, ADB_ERR_TRANSACTION, "Transaction control must be handled by a session");
    ADBTable *t = s->table ? adb_find_table(state, s->table) : NULL;
    if (!t) return adb_error(err, ADB_ERR_SCHEMA, "Unknown table '%s'", s->table ? s->table : "");
    switch (s->kind) {
    case ST_INSERT: return insert_rows(t, s, r, changed, err);
    case ST_SELECT: return select_rows(t, s, r, err);
    case ST_UPDATE: return update_rows(t, s, r, changed, err);
    case ST_DELETE: return delete_rows(t, s, r, changed, err);
    case ST_CREATE_INDEX: return create_index(state, t, s, r, changed, err);
    case ST_DESCRIBE: return describe(t, r, err);
    default: return adb_error(err, ADB_ERR_SCHEMA, "Unsupported statement");
    }
}
