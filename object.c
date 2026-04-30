// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ───────── PROVIDED ─────────

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

// ───────── IMPLEMENTATION ─────────

// Write object
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str;

    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    size_t total_size = header_len + 1 + len;
    unsigned char *full_object = malloc(total_size);
    if (!full_object) return -1;

    memcpy(full_object, header, header_len);
    full_object[header_len] = '\0';
    memcpy(full_object + header_len + 1, data, len);

    // Compute hash
    compute_hash(full_object, total_size, id_out);

    // Deduplication
    if (object_exists(id_out)) {
        free(full_object);
        return 0;
    }

    // Build path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Get directory path
    char dir_path[512];
    strncpy(dir_path, path, sizeof(dir_path));
    char *slash = strrchr(dir_path, '/');
    if (!slash) {
        free(full_object);
        return -1;
    }
    *slash = '\0';

    // Create directories
    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir_path, 0755);

    // Temp file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_object);
        return -1;
    }

    if (write(fd, full_object, total_size) != (ssize_t)total_size) {
        close(fd);
        free(full_object);
        return -1;
    }

    fsync(fd);
    close(fd);

    // Atomic rename
    if (rename(temp_path, path) != 0) {
        free(full_object);
        return -1;
    }

    // fsync directory
    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_object);
    return 0;
}

// Read object
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    rewind(f);

    unsigned char *buffer = malloc(file_size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, file_size, f) != file_size) {
        fclose(f);
        free(buffer);
        return -1;
    }
    fclose(f);

    // Integrity check
    ObjectID computed;
    compute_hash(buffer, file_size, &computed);

    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Parse header
    char *null_pos = memchr(buffer, '\0', file_size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    char type_str[16];
    size_t size;

    if (sscanf((char *)buffer, "%s %zu", type_str, &size) != 2) {
        free(buffer);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    *len_out = size;
    *data_out = malloc(size + 1);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, size);
    ((char *)(*data_out))[size] = '\0';

    free(buffer);
    return 0;
}
