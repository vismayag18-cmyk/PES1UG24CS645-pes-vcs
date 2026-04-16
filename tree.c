// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include "pes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;

    const uint8_t *ptr = data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;

        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;

        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name,
                  ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;

    uint8_t *buffer = malloc(max_size ? max_size : 1);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;

    qsort(sorted_tree.entries,
          sorted_tree.count,
          sizeof(TreeEntry),
          compare_tree_entries);

    size_t offset = 0;

    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset,
                              "%o %s",
                              entry->mode,
                              entry->name);

        offset += written + 1;

        memcpy(buffer + offset,
               entry->hash.hash,
               HASH_SIZE);

        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;

    return 0;
}

/* ---------- DEBUG RECURSIVE TREE BUILDER ---------- */

static int build_tree_recursive(Index *index,
                                const char *prefix,
                                ObjectID *id_out) {
    Tree tree = {0};

    printf("TREE DEBUG: prefix='%s'\n", prefix);

    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const char *path = index->entries[i].path;

        if (strncmp(path, prefix, prefix_len) != 0)
            continue;

        const char *remaining = path + prefix_len;

        printf("TREE DEBUG: path='%s' remaining='%s'\n",
               path, remaining);

        if (*remaining == '\0')
            continue;

        const char *slash = strchr(remaining, '/');

        if (!slash) {
            TreeEntry *entry = &tree.entries[tree.count++];

            strcpy(entry->name, remaining);
            entry->mode = index->entries[i].mode;
            entry->hash = index->entries[i].hash;
        }
        else {
            size_t dir_len = slash - remaining;

            char dirname[256];
            strncpy(dirname, remaining, dir_len);
            dirname[dir_len] = '\0';

            int already_added = 0;

            for (int j = 0; j < tree.count; j++) {
                if (strcmp(tree.entries[j].name, dirname) == 0) {
                    already_added = 1;
                    break;
                }
            }

            if (!already_added) {
                char subprefix[512];

                snprintf(subprefix,
                         sizeof(subprefix),
                         "%s%s/",
                         prefix,
                         dirname);

                ObjectID subtree_id;

                if (build_tree_recursive(index,
                                         subprefix,
                                         &subtree_id) != 0) {
                    printf("TREE FAIL: subtree recursion\n");
                    return -1;
                }

                TreeEntry *entry = &tree.entries[tree.count++];

                strcpy(entry->name, dirname);
                entry->mode = MODE_DIR;
                entry->hash = subtree_id;
            }
        }
    }

    void *serialized;
    size_t len;

    if (tree_serialize(&tree, &serialized, &len) != 0) {
        printf("TREE FAIL: serialize\n");
        return -1;
    }

    if (object_write(OBJ_TREE, serialized, len, id_out) != 0) {
        printf("TREE FAIL: object_write\n");
        free(serialized);
        return -1;
    }

    free(serialized);

    printf("TREE DEBUG: wrote tree successfully\n");

    return 0;
}

/* ---------- MAIN ---------- */

int tree_from_index(ObjectID *id_out) {
    Index index;

    printf("TREE DEBUG: loading index\n");

    if (index_load(&index) != 0) {
        printf("TREE FAIL: index_load\n");
        return -1;
    }

    printf("TREE DEBUG: index count=%d\n", index.count);

    if (build_tree_recursive(&index, "", id_out) != 0) {
        printf("TREE FAIL: build_tree_recursive\n");
        return -1;
    }

    printf("TREE DEBUG: success\n");

    return 0;
}
