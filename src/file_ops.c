#include "file_ops.h"

#include "io_utils.h"

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
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

static bool build_path(
    const usb_responder_file_ops_t* ops, const char* relative, char* out_path, size_t out_size) {
    int n = 0;

    if (!ops || !relative || !out_path || out_size == 0) {
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

static bool list_append_line(char** text, size_t* used, size_t* cap, const char* name) {
    size_t n = strlen(name) + 1;
    if (*used + n + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256u : *cap;
        while (new_cap < *used + n + 1) {
            new_cap *= 2u;
        }
        char* next = (char*)realloc(*text, new_cap);
        if (!next) {
            return false;
        }
        *text = next;
        *cap = new_cap;
    }
    memcpy(*text + *used, name, n - 1);
    (*text)[*used + n - 1] = '\n';
    *used += n;
    return true;
}

static char* list_finalize(char* text, size_t used) {
    if (!text) {
        return (char*)calloc(1, 1);
    }
    text[used] = '\0';
    return text;
}

bool usb_responder_file_list(
    const usb_responder_file_ops_t* ops, const char* relative_path, char** out_files, char** out_dirs) {
    DIR* dir = NULL;
    struct dirent* de = NULL;
    char dirpath[USB_RESPONDER_PATH_MAX_LEN];
    char* files = NULL;
    char* dirs = NULL;
    size_t fu = 0, fc = 0, du = 0, dc = 0;

    if (!ops || !relative_path || !out_files || !out_dirs) {
        return false;
    }
    *out_files = NULL;
    *out_dirs = NULL;
    if (!build_path(ops, relative_path, dirpath, sizeof(dirpath))) {
        return false;
    }
    dir = opendir(dirpath);
    if (!dir) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        char child[USB_RESPONDER_PATH_MAX_LEN];
        struct stat st;
        int n = 0;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        n = snprintf(child, sizeof(child), "%s/%s", dirpath, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) {
            closedir(dir);
            free(files);
            free(dirs);
            usb_responder_set_last_error("path too long");
            return false;
        }
        if (lstat(child, &st) != 0) {
            set_file_error("lstat", child);
            closedir(dir);
            free(files);
            free(dirs);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!list_append_line(&dirs, &du, &dc, de->d_name)) {
                closedir(dir);
                free(files);
                free(dirs);
                usb_responder_set_last_error("oom");
                return false;
            }
        } else {
            if (!list_append_line(&files, &fu, &fc, de->d_name)) {
                closedir(dir);
                free(files);
                free(dirs);
                usb_responder_set_last_error("oom");
                return false;
            }
        }
    }
    closedir(dir);

    *out_files = list_finalize(files, fu);
    *out_dirs = list_finalize(dirs, du);
    if (!*out_files || !*out_dirs) {
        free(*out_files);
        free(*out_dirs);
        *out_files = NULL;
        *out_dirs = NULL;
        usb_responder_set_last_error("oom");
        return false;
    }
    return true;
}

bool usb_responder_file_stat(
    const usb_responder_file_ops_t* ops, const char* relative_path, usb_responder_stat_info_t* out) {
    char path[USB_RESPONDER_PATH_MAX_LEN];
    struct stat st;
    char userbuf[128];
    char groupbuf[128];
    struct passwd pws;
    struct passwd* pw_ptr = NULL;
    char pwbuf[4096];
    struct group grs;
    struct group* gr_ptr = NULL;
    char grbuf[4096];

    if (!ops || !relative_path || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!build_path(ops, relative_path, path, sizeof(path))) {
        return false;
    }
    if (lstat(path, &st) != 0) {
        set_file_error("lstat", path);
        return false;
    }

    if (getpwuid_r(st.st_uid, &pws, pwbuf, sizeof(pwbuf), &pw_ptr) != 0 || pw_ptr == NULL) {
        snprintf(userbuf, sizeof(userbuf), "%u", (unsigned)st.st_uid);
    } else {
        snprintf(userbuf, sizeof(userbuf), "%s", pw_ptr->pw_name);
    }
    if (getgrgid_r(st.st_gid, &grs, grbuf, sizeof(grbuf), &gr_ptr) != 0 || gr_ptr == NULL) {
        snprintf(groupbuf, sizeof(groupbuf), "%u", (unsigned)st.st_gid);
    } else {
        snprintf(groupbuf, sizeof(groupbuf), "%s", gr_ptr->gr_name);
    }
    snprintf(out->owner, sizeof(out->owner), "%s:%s", userbuf, groupbuf);
    snprintf(out->perm, sizeof(out->perm), "%04o", (unsigned)(st.st_mode & 07777));
    snprintf(out->size, sizeof(out->size), "%llu", (unsigned long long)st.st_size);
    if (S_ISREG(st.st_mode)) {
        snprintf(out->type, sizeof(out->type), "file");
    } else if (S_ISDIR(st.st_mode)) {
        snprintf(out->type, sizeof(out->type), "dir");
    } else if (S_ISLNK(st.st_mode)) {
        snprintf(out->type, sizeof(out->type), "link");
    } else {
        snprintf(out->type, sizeof(out->type), "other");
    }
    return true;
}

static bool remove_path_recursive(const char* abs_path) {
    struct stat st;

    if (lstat(abs_path, &st) != 0) {
        set_file_error("lstat", abs_path);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(abs_path);
        struct dirent* de = NULL;
        if (!d) {
            set_file_error("opendir", abs_path);
            return false;
        }
        while ((de = readdir(d)) != NULL) {
            char child[USB_RESPONDER_PATH_MAX_LEN];
            int n = 0;
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            n = snprintf(child, sizeof(child), "%s/%s", abs_path, de->d_name);
            if (n <= 0 || (size_t)n >= sizeof(child)) {
                closedir(d);
                usb_responder_set_last_error("path too long");
                return false;
            }
            if (!remove_path_recursive(child)) {
                closedir(d);
                return false;
            }
        }
        closedir(d);
        if (rmdir(abs_path) != 0) {
            set_file_error("rmdir", abs_path);
            return false;
        }
        return true;
    }
    if (unlink(abs_path) != 0) {
        set_file_error("unlink", abs_path);
        return false;
    }
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
    return remove_path_recursive(path);
}

bool usb_responder_dir_mkdir(usb_responder_file_ops_t* ops, const char* relative_path, bool parents) {
    char path[USB_RESPONDER_PATH_MAX_LEN];
    char work[USB_RESPONDER_PATH_MAX_LEN];
    const char* r = NULL;

    if (!ops || !relative_path) {
        return false;
    }
    if (!parents) {
        if (!build_path(ops, relative_path, path, sizeof(path))) {
            return false;
        }
        if (mkdir(path, 0755) != 0) {
            set_file_error("mkdir", path);
            return false;
        }
        return true;
    }

    if (snprintf(work, sizeof(work), "%s", ops->media_root) >= (int)sizeof(work)) {
        usb_responder_set_last_error("path too long");
        return false;
    }
    r = relative_path;
    while (*r != '\0') {
        const char* slash = NULL;
        size_t seglen = 0;
        size_t wlen = 0;
        int n = 0;

        while (*r == '/') {
            ++r;
        }
        if (*r == '\0') {
            break;
        }
        slash = strchr(r, '/');
        seglen = slash ? (size_t)(slash - r) : strlen(r);
        if (seglen == 0) {
            break;
        }

        wlen = strlen(work);
        n = snprintf(work + wlen, sizeof(work) - wlen, "/%.*s", (int)seglen, r);
        if (n <= 0 || wlen + (size_t)n >= sizeof(work)) {
            usb_responder_set_last_error("path too long");
            return false;
        }

        if (mkdir(work, 0755) != 0) {
            if (errno != EEXIST) {
                set_file_error("mkdir", work);
                return false;
            }
            {
                struct stat st;
                if (stat(work, &st) != 0) {
                    set_file_error("stat", work);
                    return false;
                }
                if (!S_ISDIR(st.st_mode)) {
                    usb_responder_set_last_error("path exists and is not a directory");
                    return false;
                }
            }
        }
        r = slash ? slash + 1 : "";
    }
    return true;
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
