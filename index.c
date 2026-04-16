// index.c — Staging area implementation

#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;

            if (remaining > 0) {
                memmove(&index->entries[i],
                        &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            }

            index->count--;
            return index_save(index);
        }
    }

    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");

    if (index->count == 0) {
        printf("  (nothing to show)\n");
    } else {
        for (int i = 0; i < index->count; i++) {
            printf("  staged:     %s\n", index->entries[i].path);
        }
    }

    printf("\nUnstaged changes:\n");
    int unstaged = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;

        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged++;
        }
        else if ((uint64_t)st.st_mtime != index->entries[i].mtime_sec ||
                 (uint32_t)st.st_size != index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            unstaged++;
        }
    }

    if (!unstaged) {
        printf("  (nothing to show)\n");
    }

    printf("\nUntracked files:\n");
    int untracked = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;

        while ((ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") ||
                !strcmp(ent->d_name, "..") ||
                !strcmp(ent->d_name, ".pes") ||
                !strcmp(ent->d_name, "pes") ||
                strstr(ent->d_name, ".o"))
                continue;

            int tracked = 0;

            for (int i = 0; i < index->count; i++) {
                if (!strcmp(index->entries[i].path, ent->d_name)) {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked) {
                struct stat st;

                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked++;
                }
            }
        }

        closedir(dir);
    }

    if (!untracked) {
        printf("  (nothing to show)\n");
    }

    printf("\n");
    return 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    index->count = 0;

    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) return 0;

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *entry = &index->entries[index->count];

        char hash_hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime;
        unsigned int size;

        int read = fscanf(fp,
            "%o %64s %llu %u %511[^\n]\n",
            &mode,
            hash_hex,
            &mtime,
            &size,
            entry->path);

        if (read != 5)
            break;

        entry->mode = mode;
        entry->mtime_sec = mtime;
        entry->size = size;

        if (hex_to_hash(hash_hex, &entry->hash) != 0) {
            fclose(fp);
            return -1;
        }

        index->count++;
    }

    fclose(fp);
    return 0;
}

int index_save(const Index *index) {
    Index *mutable_index = (Index *)index;

    qsort(mutable_index->entries,
          mutable_index->count,
          sizeof(IndexEntry),
          compare_index_entries);

    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) return -1;

    for (int i = 0; i < mutable_index->count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&mutable_index->entries[i].hash, hash_hex);

        fprintf(fp, "%o %s %" PRIu64 " %u %s\n",
                mutable_index->entries[i].mode,
                hash_hex,
                mutable_index->entries[i].mtime_sec,
                mutable_index->entries[i].size,
                mutable_index->entries[i].path);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(".pes/index.tmp", INDEX_FILE) != 0)
        return -1;

    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    size_t file_size = (size_t)st.st_size;

    void *buffer = malloc(file_size ? file_size : 1);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    if (file_size > 0 &&
        fread(buffer, 1, file_size, fp) != file_size) {
        free(buffer);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    ObjectID blob_id;

    if (object_write(OBJ_BLOB, buffer, file_size, &blob_id) != 0) {
        free(buffer);
        return -1;
    }

    free(buffer);

    IndexEntry *entry = index_find(index, path);

    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;

        entry = &index->entries[index->count++];
    }

    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';

    entry->mode = get_file_mode(path);
    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;

    return index_save(index);
}
