// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6... 1699900000 42 README.md
//   100644 f7e8d9c0b1a2... 1699900100 128 src/main.c
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions: index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Comparison function for qsort — sort entries by path lexicographically.
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Load the index from .pes/index.
// If the file doesn't exist, initialize an empty index (not an error).
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    memset(index, 0, sizeof(*index));

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // No index file yet — that's fine, we have an empty index.
        return 0;
    }

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    uint64_t mtime;
    uint64_t size;
    char path[MAX_PATH_LEN];

    while (index->count < MAX_INDEX_ENTRIES) {
        // Format: <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
        int ret = fscanf(f, "%o %64s %llu %llu %4095s\n",
                         &mode,
                         hex,
                         (unsigned long long *)&mtime,
                         (unsigned long long *)&size,
                         path);
        if (ret == EOF) break;
        if (ret != 5) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count];
        e->mode = mode;
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        e->mtime_sec = mtime;
        e->size = size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically (temp file + rename).
// Entries are sorted by path before writing.
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Make a sorted copy of the entries
    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_index_entries);

    // Write to a temp file first
    char tmp_path[] = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %llu %s\n",
                e->mode,
                hex,
                (unsigned long long)e->mtime_sec,
                (unsigned long long)e->size,
                e->path);
    }

    // Flush userspace buffers, then sync to disk
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // Atomically move the temp file over the old index
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
// Reads the file, writes it as a blob object, then updates the index entry.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Step 1: Read the file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    void *contents = malloc((size_t)file_size + 1);
    if (!contents) {
        fclose(f);
        return -1;
    }

    if (file_size > 0 && fread(contents, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(contents);
        return -1;
    }
    fclose(f);

    // Step 2: Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    // Step 3: Get file metadata (mtime, size, mode)
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    uint32_t mode;
    if (st.st_mode & S_IXUSR)
        mode = 0100755;
    else
        mode = 0100644;

    // Step 4: Update or add the index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        // Update in place
        existing->hash     = blob_id;
        existing->mode     = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size     = (uint64_t)st.st_size;
    } else {
        // Add a new entry
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        IndexEntry *ne = &index->entries[index->count];
        ne->hash      = blob_id;
        ne->mode      = mode;
        ne->mtime_sec = (uint64_t)st.st_mtime;
        ne->size      = (uint64_t)st.st_size;
        strncpy(ne->path, path, sizeof(ne->path) - 1);
        ne->path[sizeof(ne->path) - 1] = '\0';
        index->count++;
    }

    // Step 5: Save the updated index
    return index_save(index);
}

