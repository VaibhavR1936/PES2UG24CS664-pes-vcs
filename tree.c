// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ───────────────────────────────────────────────────────────

// Forward declarations from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Recursive helper: builds a tree for entries[0..count) where each path
// is relative (the leading prefix up to this depth has already been consumed).
//
// 'entries' is an array of IndexEntry pointers whose paths start at 'depth'
// (i.e., if depth == 1, path[0] is already past the first directory component).
//
// prefix_len: number of characters already consumed from the path
// Returns 0 on success, -1 on error.
static int write_tree_level(IndexEntry *entries, int count, int prefix_len, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *rel_path = entries[i].path + prefix_len;
        char *slash = strchr(rel_path, '/');

        if (slash == NULL) {
            // This is a file at this level
            TreeEntry *te = &tree.entries[tree.count];
            strncpy(te->name, rel_path, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            tree.count++;
            i++;
        } else {
            // This is a subdirectory: collect all entries with the same prefix
            size_t dir_len = (size_t)(slash - rel_path);
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel_path, dir_len);
            dir_name[dir_len] = '\0';

            // Find the range of entries in this subdirectory
            int j = i + 1;
            while (j < count) {
                const char *p = entries[j].path + prefix_len;
                // Check if same leading directory component
                if (strncmp(p, dir_name, dir_len) == 0 && p[dir_len] == '/') {
                    j++;
                } else {
                    break;
                }
            }

            // Recursively write the subtree for entries[i..j)
            int new_prefix = prefix_len + (int)dir_len + 1; // skip "dir/"
            ObjectID subtree_id;
            if (write_tree_level(entries + i, j - i, new_prefix, &subtree_id) != 0)
                return -1;

            // Add the directory entry at this level
            TreeEntry *te = &tree.entries[tree.count];
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            tree.count++;

            i = j;
        }
    }

    // Serialize and write the tree object
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    memset(&index, 0, sizeof(index));

    if (index_load(&index) != 0) return -1;

    if (index.count == 0) {
        // Empty tree: serialize an empty Tree struct
        Tree empty;
        empty.count = 0;
        void *tree_data;
        size_t tree_len;
        if (tree_serialize(&empty, &tree_data, &tree_len) != 0) return -1;
        int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
        free(tree_data);
        return rc;
    }

    return write_tree_level(index.entries, index.count, 0, id_out);
}

