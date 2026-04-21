// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions: object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ──────────────────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build the full object: header + data
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Build header: "<type> <size>\0"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1; 

    // Allocate full object buffer
    size_t full_len = (size_t)header_len + len;
    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;
    memcpy(full_obj, header, header_len);
    memcpy(full_obj + header_len, data, len);

    // Step 2: Compute SHA-256 of the full object
    compute_hash(full_obj, full_len, id_out);

    // Step 3: Check if object already exists (deduplication)
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // Step 4: Create shard directory (.pes/objects/XX/) if it doesn't exist
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // OK if it already exists

    // Step 5: Determine final object path and a temp path
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    // Step 6: Write to temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0444);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    ssize_t written = write(fd, full_obj, full_len);
    free(full_obj);

    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // Step 7: fsync the temp file to ensure data reaches disk
    fsync(fd);
    close(fd);

    // Step 8: rename() the temp file to the final path (atomic on POSIX)
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // Step 9: fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
  return 0;
}

// Read an object from the store.
//
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
// The caller is responsible for calling free(*data_out).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Build the file path from the hash
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *raw = malloc((size_t)file_size);
    if (!raw) {
        fclose(f);
        return -1;
    }

    if (fread(raw, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(raw);
        return -1;
    }
    fclose(f);

    // Step 3: Verify integrity — recompute hash and compare to the filename
    ObjectID computed;
    compute_hash(raw, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(raw);
        return -1; // Corrupted object
    }

    // Step 4: Parse the header to extract type and size
    // Header format: "<type> <size>\0"
    uint8_t *null_byte = memchr(raw, '\0', (size_t)file_size);
    if (!null_byte) {
        free(raw);
        return -1;
    }

    // Interpret header as a C string
    char *header = (char *)raw;

    if (strncmp(header, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp(header, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp(header, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else {
        free(raw);
        return -1;
    }

    // Parse size from header
    char *size_str = strchr(header, ' ') + 1;
    size_t data_size = (size_t)strtoul(size_str, NULL, 10);

    // Step 5: Copy the data portion (after the '\0')
    uint8_t *data_start = null_byte + 1;
    size_t remaining = (size_t)file_size - (size_t)(data_start - raw);

    if (remaining != data_size) {
        free(raw);
        return -1;
    }

    uint8_t *data_copy = malloc(data_size + 1);
    if (!data_copy) {
        free(raw);
        return -1;
    }

    memcpy(data_copy, data_start, data_size);
    data_copy[data_size] = '\0'; // null-terminate for convenience (text objects)

    free(raw);

    *data_out = data_copy;
    *len_out = data_size;
    
    return 0;
}

