// Write an object to the store
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str;

    // Step 1: Type string
    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    // Step 2: Header
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    // Step 3: Full object = header + '\0' + data
    size_t total_size = header_len + 1 + len;
    unsigned char *full_object = malloc(total_size);
    if (!full_object) return -1;

    memcpy(full_object, header, header_len);
    full_object[header_len] = '\0';
    memcpy(full_object + header_len + 1, data, len);

    // Step 4: Compute hash
    compute_hash(full_object, total_size, id_out);

    // Step 5: Deduplication
    if (object_exists(id_out)) {
        free(full_object);
        return 0;
    }

    // Step 6: Build object path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Extract directory path
    char dir_path[512];
    strncpy(dir_path, path, sizeof(dir_path));
    char *slash = strrchr(dir_path, '/');
    if (!slash) {
        free(full_object);
        return -1;
    }
    *slash = '\0';

    // Step 7: Create shard directory
    mkdir(OBJECTS_DIR, 0755);   // safe even if exists
    mkdir(dir_path, 0755);

    // Step 8: Temp file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_object);
        return -1;
    }

    // Step 9: Write data
    if (write(fd, full_object, total_size) != (ssize_t)total_size) {
        close(fd);
        free(full_object);
        return -1;
    }

    // Step 10: fsync file
    fsync(fd);
    close(fd);

    // Step 11: Atomic rename
    if (rename(temp_path, path) != 0) {
        free(full_object);
        return -1;
    }

    // Step 12: fsync directory
    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_object);
    return 0;
}


// Read an object from the store
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 1: Open file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // Step 2: Get file size
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

    // Step 3: Integrity check
    ObjectID computed;
    compute_hash(buffer, file_size, &computed);

    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Step 4: Parse header
    char *null_pos = memchr(buffer, '\0', file_size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    size_t header_len = null_pos - (char *)buffer;

    char type_str[16];
    size_t size;

    if (sscanf((char *)buffer, "%s %zu", type_str, &size) != 2) {
        free(buffer);
        return -1;
    }

    // Step 5: Set type
    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    // Step 6: Extract data
    *len_out = size;
    *data_out = malloc(size);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, size);

    free(buffer);
    return 0;
}
