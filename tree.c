// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
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
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

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
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Implementation: tree_from_index ────────────────────────────────────────

// Forward declaration: object_write is in object.c (always linked with tree.o).
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Internal index reading ─────────────────────────────────────────────────────
//
// We intentionally avoid a link-time dependency on index.c so that the
// test_tree binary (which links only tree.o + object.o) still builds.
// The index file format is trivial enough to parse inline.

#define TREE_MAX_INDEX_ENTRIES 10000

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char     path[512];
} _TFlatEntry;

// Read the text index file (.pes/index) and populate a FlatEntry array.
// Returns the number of entries read, or -1 on error.
static int _load_flat_entries(_TFlatEntry *out, int max_entries) {
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;   // No index yet — empty is valid

    int count = 0;
    char hex[HASH_HEX_SIZE + 1];
    uint32_t mode;
    unsigned long long mtime;
    uint32_t size;
    char path[512];

    while (count < max_entries &&
           fscanf(f, "%o %64s %llu %u %511s\n",
                  &mode, hex, &mtime, &size, path) == 5) {
        out[count].mode = mode;
        strncpy(out[count].path, path, sizeof(out[count].path) - 1);
        out[count].path[sizeof(out[count].path) - 1] = '\0';
        if (hex_to_hash(hex, &out[count].hash) != 0) {
            fclose(f);
            return -1;
        }
        count++;
    }

    fclose(f);
    return count;
}

// Comparison for qsort — keeps entries sorted by path
static int _cmp_flat(const void *a, const void *b) {
    return strcmp(((_TFlatEntry *)a)->path, ((_TFlatEntry *)b)->path);
}

// Recursive helper: given an array of _TFlatEntry items (all paths relative
// to the current tree level), build one tree object and write it to the
// object store.
static int _write_tree_level(const _TFlatEntry *entries, int count, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;
        const char *slash = strchr(path, '/');

        if (!slash) {
            // Plain file at this level
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, path, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // Subdirectory: gather all entries sharing this dir prefix
            size_t prefix_len = (size_t)(slash - path);
            char dir_name[256];
            if (prefix_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, path, prefix_len);
            dir_name[prefix_len] = '\0';

            // Collect entries belonging to this subdirectory
            int sub_start = i;
            while (i < count) {
                const char *p = entries[i].path;
                const char *s = strchr(p, '/');
                if (!s) break;
                size_t plen = (size_t)(s - p);
                if (plen != prefix_len || strncmp(p, dir_name, prefix_len) != 0) break;
                i++;
            }
            int sub_count = i - sub_start;

            // Build FlatEntry array with the leading dir component stripped
            _TFlatEntry *sub = malloc((size_t)sub_count * sizeof(_TFlatEntry));
            if (!sub) return -1;

            for (int j = 0; j < sub_count; j++) {
                sub[j].mode = entries[sub_start + j].mode;
                sub[j].hash = entries[sub_start + j].hash;
                const char *rest = entries[sub_start + j].path + prefix_len + 1;
                strncpy(sub[j].path, rest, sizeof(sub[j].path) - 1);
                sub[j].path[sizeof(sub[j].path) - 1] = '\0';
            }

            // Recurse
            ObjectID subtree_id;
            if (_write_tree_level(sub, sub_count, &subtree_id) != 0) {
                free(sub);
                return -1;
            }
            free(sub);

            // Add directory entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000;
            te->hash = subtree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        }
    }

    // Serialise and store this tree level
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    _TFlatEntry *flat = malloc(TREE_MAX_INDEX_ENTRIES * sizeof(_TFlatEntry));
    if (!flat) return -1;

    int count = _load_flat_entries(flat, TREE_MAX_INDEX_ENTRIES);
    if (count <= 0) {
        free(flat);
        return -1;   // Nothing staged
    }

    // Sort by path (index_save already sorts, but be safe)
    qsort(flat, count, sizeof(_TFlatEntry), _cmp_flat);

    int rc = _write_tree_level(flat, count, id_out);
    free(flat);
    return rc;
}