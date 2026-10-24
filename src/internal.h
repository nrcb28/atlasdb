#ifndef ADB_INTERNAL_H
#define ADB_INTERNAL_H

#include "atlasdb.h"
#include <pthread.h>
#include <stdarg.h>

#define ADB_MAX_TABLES 1024u
#define ADB_MAX_ROWS 1000000u
#define ADB_MAX_EXPR_DEPTH 128u
#define ADB_MAX_FRAME (256u * 1024u * 1024u)

bool adb_error(ADBError *error, ADBCode code, const char *format, ...);
void *adb_alloc(size_t count, size_t size, ADBError *error);
char *adb_strdup(const char *text, ADBError *error);
bool adb_name_equal(const char *left, const char *right);
bool adb_utf8_valid(const char *text);
ADBValue adb_null(void);
bool adb_value_copy(ADBValue *dest, const ADBValue *source, ADBError *error);
int adb_value_compare(const ADBValue *a, const ADBValue *b);
bool adb_value_coerce(ADBValue *value, ADBType type, ADBError *error);
bool adb_value_truth(const ADBValue *value);
char *adb_value_string(const ADBValue *value);
bool adb_result_columns(ADBResult *result, size_t count, const char *const *names, ADBError *error);
bool adb_result_row(ADBResult *result, const ADBValue *values, ADBError *error);
bool adb_result_message(ADBResult *result, const char *format, ...);

typedef struct { char *data; size_t length; size_t capacity; bool failed; } ADBBuffer;
bool adb_buffer_append(ADBBuffer *buffer, const void *data, size_t length);
bool adb_buffer_printf(ADBBuffer *buffer, const char *format, ...);
bool adb_buffer_json_string(ADBBuffer *buffer, const char *text);
void adb_buffer_free(ADBBuffer *buffer);

/* SQL abstract syntax tree. Parser owns every string, value, and child node. */
typedef enum { EX_LITERAL, EX_COLUMN, EX_UNARY, EX_BINARY, EX_FUNCTION, EX_STAR } ADBExprKind;
typedef enum {
    OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EQ, OP_NE,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_AND, OP_OR, OP_NOT, OP_NEG,
    OP_IS_NULL, OP_IS_NOT_NULL, OP_LIKE, OP_NOT_LIKE
} ADBOp;
typedef struct ADBExpr {
    ADBExprKind kind;
    ADBOp op;
    ADBValue literal;
    char *name;
    struct ADBExpr *left, *right;
} ADBExpr;
typedef struct {
    char *name;
    ADBType type;
    bool primary_key, unique, not_null, has_default;
    ADBValue default_value;
} ADBColumn;
typedef struct { ADBExpr *expression; char *alias; } ADBProjection;
typedef struct { char *column; ADBExpr *value; } ADBAssignment;
typedef enum {
    ST_CREATE_TABLE, ST_DROP_TABLE, ST_INSERT, ST_SELECT, ST_UPDATE,
    ST_DELETE, ST_CREATE_INDEX, ST_DROP_INDEX, ST_SHOW_TABLES, ST_DESCRIBE,
    ST_BEGIN, ST_COMMIT, ST_ROLLBACK, ST_CHECKPOINT, ST_EXPLAIN
} ADBStatementKind;
typedef struct ADBStatement {
    ADBStatementKind kind;
    char *table, *index_name;
    bool if_exists, if_not_exists, unique;
    ADBColumn *columns;
    size_t ncolumns;
    char **insert_columns;
    size_t ninsert_columns;
    ADBExpr ***values;
    size_t *value_counts;
    size_t nvalue_rows;
    ADBProjection *projections;
    size_t nprojections;
    ADBExpr *where;
    ADBAssignment *assignments;
    size_t nassignments;
    char *order_column;
    bool order_desc;
    uint64_t limit, offset;
    bool has_limit;
    struct ADBStatement *inner;
} ADBStatement;
ADBStatement *adb_parse(const char *sql, ADBError *error);
void adb_statement_free(ADBStatement *statement);
void adb_expr_free(ADBExpr *expression);

/* Ordered AVL multimap used by persistent index definitions. */
typedef struct ADBTree ADBTree;
ADBTree *adb_tree_create(ADBError *error);
void adb_tree_free(ADBTree *tree);
bool adb_tree_insert(ADBTree *tree, const ADBValue *key, uint64_t row_id, bool unique, ADBError *error);
const uint64_t *adb_tree_find(const ADBTree *tree, const ADBValue *key, size_t *count);
typedef struct { char *name; size_t column; bool unique; bool implicit; ADBTree *tree; } ADBIndex;
typedef struct { uint64_t id; ADBValue *values; } ADBRow;
typedef struct {
    char *name;
    ADBColumn *columns;
    size_t ncolumns;
    ADBRow *rows;
    size_t nrows, row_capacity;
    uint64_t next_row_id;
    ADBIndex *indexes;
    size_t nindexes;
} ADBTable;
typedef struct { ADBTable *tables; size_t ntables; uint64_t version; } ADBState;
void adb_column_free(ADBColumn *column);
bool adb_column_copy(ADBColumn *dest, const ADBColumn *source, ADBError *error);
void adb_table_free(ADBTable *table);
void adb_state_free(ADBState *state);
ADBState *adb_state_create(ADBError *error);
ADBState *adb_state_clone(const ADBState *state, ADBError *error);
ADBTable *adb_find_table(ADBState *state, const char *name);
int adb_find_column(const ADBTable *table, const char *name);
bool adb_table_reindex(ADBTable *table, ADBError *error);

/* Execution mutates its private state. Caller discards it on any error. */
bool adb_engine_execute(ADBState *state, const ADBStatement *statement, ADBResult *result, bool *changed, ADBError *error);
bool adb_statement_writes(const ADBStatement *statement);

/* Durable storage: checksummed full-state WAL records and atomic checkpoint. */
typedef struct { char *directory; int lock_fd; int wal_fd; bool memory; } ADBStorage;
bool adb_storage_open(ADBStorage *storage, const char *directory, ADBState **state, ADBError *error);
void adb_storage_close(ADBStorage *storage);
bool adb_storage_commit(ADBStorage *storage, const ADBState *state, ADBError *error);
bool adb_storage_checkpoint(ADBStorage *storage, const ADBState *state, ADBError *error);
bool adb_state_encode(const ADBState *state, unsigned char **data, size_t *length, ADBError *error);
ADBState *adb_state_decode(const unsigned char *data, size_t length, ADBError *error);
uint32_t adb_crc32(const void *data, size_t length);

struct ADBDatabase { pthread_mutex_t mutex; ADBState *state; ADBStorage storage; };
struct ADBSession { ADBDatabase *database; ADBState *transaction; uint64_t base_version; bool dirty; };

/* Frontends share SQL script framing and bounded TCP protocol. */
int adb_run_server(ADBDatabase *database, const char *host, unsigned port, size_t max_clients);
int adb_run_client(const char *host, unsigned port, const char *sql);

#endif
