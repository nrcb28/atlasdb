#include "internal.h"

#include <stdlib.h>

/* Keys and row-id vectors belong to each node. Rotations move node pointers,
 * never keys, so callers can retain lookup vectors until the tree is changed. */
typedef struct ADBTreeNode {
    ADBValue key;
    uint64_t *rows;
    size_t count, capacity;
    int height;
    struct ADBTreeNode *left, *right;
} ADBTreeNode;

struct ADBTree {
    ADBTreeNode *root;
    size_t entries;
};

static int height(const ADBTreeNode *node)
{
    return node ? node->height : 0;
}

static void update_height(ADBTreeNode *node)
{
    int left = height(node->left), right = height(node->right);
    node->height = 1 + (left > right ? left : right);
}

static ADBTreeNode *rotate_right(ADBTreeNode *root)
{
    ADBTreeNode *pivot = root->left;
    root->left = pivot->right;
    pivot->right = root;
    update_height(root);
    update_height(pivot);
    return pivot;
}

static ADBTreeNode *rotate_left(ADBTreeNode *root)
{
    ADBTreeNode *pivot = root->right;
    root->right = pivot->left;
    pivot->left = root;
    update_height(root);
    update_height(pivot);
    return pivot;
}

static ADBTreeNode *balance(ADBTreeNode *root)
{
    update_height(root);
    int difference = height(root->left) - height(root->right);
    if (difference > 1) {
        if (height(root->left->left) < height(root->left->right))
            root->left = rotate_left(root->left);
        return rotate_right(root);
    }
    if (difference < -1) {
        if (height(root->right->right) < height(root->right->left))
            root->right = rotate_right(root->right);
        return rotate_left(root);
    }
    return root;
}

static void node_free(ADBTreeNode *node)
{
    if (!node) return;
    node_free(node->left);
    node_free(node->right);
    adb_value_free(&node->key);
    free(node->rows);
    free(node);
}

ADBTree *adb_tree_create(ADBError *error)
{
    return adb_alloc(1, sizeof(ADBTree), error);
}

void adb_tree_free(ADBTree *tree)
{
    if (!tree) return;
    node_free(tree->root);
    free(tree);
}

static ADBTreeNode *node_create(const ADBValue *key, uint64_t row_id,
                              ADBError *error)
{
    ADBTreeNode *node = adb_alloc(1, sizeof(*node), error);
    if (!node) return NULL;
    if (!adb_value_copy(&node->key, key, error)) {
        free(node);
        return NULL;
    }
    node->rows = adb_alloc(1, sizeof(*node->rows), error);
    if (!node->rows) {
        adb_value_free(&node->key);
        free(node);
        return NULL;
    }
    node->rows[0] = row_id;
    node->count = node->capacity = 1;
    node->height = 1;
    return node;
}

static bool insert_node(ADBTreeNode **position, const ADBValue *key,
                        uint64_t row_id, bool unique, ADBError *error)
{
    ADBTreeNode *node = *position;
    if (!node) {
        *position = node_create(key, row_id, error);
        return *position != NULL;
    }
    int comparison = adb_value_compare(key, &node->key);
    if (comparison == 0) {
        /* SQL UNIQUE permits multiple NULLs; PRIMARY KEY is checked by model. */
        if (unique && key->type != ADB_NULL)
            return adb_error(error, ADB_ERR_CONSTRAINT, "duplicate value in unique index");
        if (node->count == node->capacity) {
            size_t capacity = node->capacity * 2;
            if (capacity > ADB_MAX_ROWS) capacity = ADB_MAX_ROWS;
            if (capacity <= node->count)
                return adb_error(error, ADB_ERR_LIMIT, "index row limit exceeded");
            uint64_t *rows = realloc(node->rows, capacity * sizeof(*rows));
            if (!rows)
                return adb_error(error, ADB_ERR_MEMORY, "cannot grow index row vector");
            node->rows = rows;
            node->capacity = capacity;
        }
        node->rows[node->count++] = row_id;
        return true;
    }
    ADBTreeNode **child = comparison < 0 ? &node->left : &node->right;
    if (!insert_node(child, key, row_id, unique, error)) return false;
    *position = balance(node);
    return true;
}

bool adb_tree_insert(ADBTree *tree, const ADBValue *key, uint64_t row_id,
                     bool unique, ADBError *error)
{
    if (!tree || !key)
        return adb_error(error, ADB_ERR_SCHEMA, "invalid index insertion");
    if (tree->entries >= ADB_MAX_ROWS)
        return adb_error(error, ADB_ERR_LIMIT, "index row limit exceeded");
    if (!insert_node(&tree->root, key, row_id, unique, error)) return false;
    tree->entries++;
    return true;
}

const uint64_t *adb_tree_find(const ADBTree *tree, const ADBValue *key,
                              size_t *count)
{
    if (count) *count = 0;
    if (!tree || !key) return NULL;
    const ADBTreeNode *node = tree->root;
    while (node) {
        int comparison = adb_value_compare(key, &node->key);
        if (comparison == 0) {
            if (count) *count = node->count;
            return node->rows;
        }
        node = comparison < 0 ? node->left : node->right;
    }
    return NULL;
}
