#include "file_ops.h"

#include "io_utils.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_file_error(const char* prefix, const char* path) {
    char msg[512];
    snprintf(msg, sizeof(msg), "%s %s: %s", prefix, path ? path : "", strerror(errno));
    usb_responder_set_last_error(msg);
}

static bool validate_relative_path(const char* path) {
    const char* p = path;

    if (!p || p[0] == '\0') {
        usb_responder_set_last_error("invalid empty path");
        return false;
    }
    if (p[0] == '/') {
        usb_responder_set_last_error("absolute paths are not allowed");
        return false;
    }
    while (*p != '\0') {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            usb_responder_set_last_error("path traversal is not allowed");
            return false;
        }
        ++p;
    }
    return true;
}

static bool build_path(
    const usb_responder_file_ops_t* ops, const char* relative, char* out_path, size_t out_size) {
    int n = 0;

    if (!ops || !relative || !out_path || out_size == 0 || !validate_relative_path(relative)) {
        return false;
    }
    n = snprintf(out_path, out_size, "%s/%s", ops->media_root, relative);
    if (n <= 0 || (size_t)n >= out_size) {
        usb_responder_set_last_error("path too long");
        return false;
    }
    return true;
}

static usb_responder_upload_session_t* find_session(usb_responder_file_ops_t* ops, uint32_t transfer_id) {
    for (size_t i = 0; i < USB_RESPONDER_MAX_UPLOAD_SESSIONS; ++i) {
        if (ops->sessions[i].used && ops->sessions[i].transfer_id == transfer_id) {
            return &ops->sessions[i];
        }
    }
    return NULL;
}

static usb_responder_upload_session_t* alloc_session(usb_responder_file_ops_t* ops) {
    for (size_t i = 0; i < USB_RESPONDER_MAX_UPLOAD_SESSIONS; ++i) {
        if (!ops->sessions[i].used) {
            ops->sessions[i].used = true;
            return &ops->sessions[i];
        }
    }
    return NULL;
}

bool usb_responder_file_ops_init(usb_responder_file_ops_t* ops, const char* media_root) {
    if (!ops || !media_root || media_root[0] == '\0') {
        usb_responder_set_last_error("invalid media root");
        return false;
    }
    memset(ops, 0, sizeof(*ops));
    if (snprintf(ops->media_root, sizeof(ops->media_root), "%s", media_root) >=
        (int)sizeof(ops->media_root)) {
        usb_responder_set_last_error("media root too long");
        return false;
    }
    return true;
}

void usb_responder_file_ops_shutdown(usb_responder_file_ops_t* ops) {
    if (!ops) {
        return;
    }
    for (size_t i = 0; i < USB_RESPONDER_MAX_UPLOAD_SESSIONS; ++i) {
        if (ops->sessions[i].used) {
            if (ops->sessions[i].fp) {
                fclose(ops->sessions[i].fp);
            }
            unlink(ops->sessions[i].temp_path);
        }
    }
    memset(ops->sessions, 0, sizeof(ops->sessions));
}

bool usb_responder_file_begin_upload(
    usb_responder_file_ops_t* ops, uint32_t transfer_id, const char* relative_path) {
    usb_responder_upload_session_t* session = NULL;

    if (!ops || !relative_path || find_session(ops, transfer_id)) {
        usb_responder_set_last_error("invalid upload session");
        return false;
    }
    session = alloc_session(ops);
    if (!session) {
        usb_responder_set_last_error("no free upload session");
        return false;
    }
    if (!build_path(ops, relative_path, session->final_path, sizeof(session->final_path))) {
        session->used = false;
        return false;
    }
    if (snprintf(session->relative_path, sizeof(session->relative_path), "%s", relative_path) >=
        (int)sizeof(session->relative_path)) {
        session->used = false;
        usb_responder_set_last_error("relative path too long");
        return false;
    }
    if (snprintf(session->temp_path, sizeof(session->temp_path), "%s.part", session->final_path) >=
        (int)sizeof(session->temp_path)) {
        session->used = false;
        usb_responder_set_last_error("temp path too long");
        return false;
    }

    session->fp = fopen(session->temp_path, "wb");
    if (!session->fp) {
        session->used = false;
        set_file_error("fopen", session->temp_path);
        return false;
    }
    session->transfer_id = transfer_id;
    session->bytes_written = 0;
    return true;
}

bool usb_responder_file_append_upload_chunk(
    usb_responder_file_ops_t* ops, uint32_t transfer_id, const uint8_t* data, size_t size) {
    usb_responder_upload_session_t* session = NULL;
    if (!ops || !data || size == 0) {
        usb_responder_set_last_error("invalid upload chunk");
        return false;
    }
    session = find_session(ops, transfer_id);
    if (!session || !session->fp) {
        usb_responder_set_last_error("upload session not found");
        return false;
    }
    if (fwrite(data, 1, size, session->fp) != size) {
        set_file_error("fwrite", session->temp_path);
        return false;
    }
    session->bytes_written += size;
    return true;
}

bool usb_responder_file_finish_upload(usb_responder_file_ops_t* ops, uint32_t transfer_id) {
    usb_responder_upload_session_t* session = NULL;
    if (!ops) {
        usb_responder_set_last_error("invalid file ops");
        return false;
    }
    session = find_session(ops, transfer_id);
    if (!session) {
        usb_responder_set_last_error("upload session not found");
        return false;
    }
    if (session->fp) {
        fclose(session->fp);
        session->fp = NULL;
    }
    if (rename(session->temp_path, session->final_path) != 0) {
        set_file_error("rename", session->final_path);
        unlink(session->temp_path);
        memset(session, 0, sizeof(*session));
        return false;
    }
    memset(session, 0, sizeof(*session));
    return true;
}

bool usb_responder_file_abort_upload(usb_responder_file_ops_t* ops, uint32_t transfer_id) {
    usb_responder_upload_session_t* session = NULL;
    if (!ops) {
        return false;
    }
    session = find_session(ops, transfer_id);
    if (!session) {
        return false;
    }
    if (session->fp) {
        fclose(session->fp);
        session->fp = NULL;
    }
    unlink(session->temp_path);
    memset(session, 0, sizeof(*session));
    return true;
}

bool usb_responder_file_read(
    const usb_responder_file_ops_t* ops, const char* relative_path, uint8_t** out_data, size_t* out_size) {
    char path[USB_RESPONDER_PATH_MAX_LEN];
    if (!ops || !relative_path || !out_data || !out_size) {
        return false;
    }
    if (!build_path(ops, relative_path, path, sizeof(path))) {
        return false;
    }
    return usb_responder_read_file_all(path, out_data, out_size);
}

bool usb_responder_file_list(
    const usb_responder_file_ops_t* ops, const char* relative_path, char** out_text) {
    DIR* dir = NULL;
    struct dirent* de = NULL;
    char path[USB_RESPONDER_PATH_MAX_LEN];
    char* text = NULL;
    size_t used = 0;
    size_t cap = 0;

    if (!ops || !relative_path || !out_text) {
        return false;
    }
    if (!build_path(ops, relative_path, path, sizeof(path))) {
        return false;
    }
    dir = opendir(path);
    if (!dir) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        size_t n = strlen(de->d_name) + 1;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (used + n + 1 > cap) {
            size_t new_cap = (cap == 0) ? 256u : cap;
            while (new_cap < used + n + 1) {
                new_cap *= 2u;
            }
            char* next = (char*)realloc(text, new_cap);
            if (!next) {
                free(text);
                closedir(dir);
                return false;
            }
            text = next;
            cap = new_cap;
        }
        memcpy(text + used, de->d_name, n - 1);
        text[used + n - 1] = '\n';
        used += n;
    }
    closedir(dir);

    if (!text) {
        text = (char*)calloc(1, 1);
        if (!text) {
            return false;
        }
    } else {
        text[used] = '\0';
    }
    *out_text = text;
    return true;
}

bool usb_responder_file_delete(const usb_responder_file_ops_t* ops, const char* relative_path) {
    char path[USB_RESPONDER_PATH_MAX_LEN];
    if (!ops || !relative_path) {
        return false;
    }
    if (!build_path(ops, relative_path, path, sizeof(path))) {
        return false;
    }
    return unlink(path) == 0;
}

bool usb_responder_file_rename(
    const usb_responder_file_ops_t* ops, const char* from_relative, const char* to_relative) {
    char from_path[USB_RESPONDER_PATH_MAX_LEN];
    char to_path[USB_RESPONDER_PATH_MAX_LEN];
    if (!ops || !from_relative || !to_relative) {
        return false;
    }
    if (!build_path(ops, from_relative, from_path, sizeof(from_path)) ||
        !build_path(ops, to_relative, to_path, sizeof(to_path))) {
        return false;
    }
    return rename(from_path, to_path) == 0;
}
