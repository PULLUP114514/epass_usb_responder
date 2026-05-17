#include "responder.h"

#include "file_ops.h"
#include "functionfs_descriptors.h"
#include "io_utils.h"
#include "protocol.h"
#include "shell_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <unistd.h>

#define USB_RESPONDER_MAX_PAYLOAD (8u * 1024u * 1024u)
#define USB_RESPONDER_IO_CHUNK (16u * 1024u)
#define USB_RESPONDER_RX_WAIT_LOG_MS 5000

typedef struct responder_runtime {
    usb_responder_config_t cfg;
    usb_responder_file_ops_t files;
    int ep0_fd;
    int ep_in_fd;
    int ep_out_fd;
    uint8_t* rx_buf;
    size_t rx_size;
    size_t rx_cap;
    bool ep_shutdown;
} responder_runtime_t;

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static const char* msg_type_name(uint16_t type) {
    switch ((usb_responder_msg_type_t)type) {
    case USB_RESPONDER_MSG_HELLO:
        return "HELLO";
    case USB_RESPONDER_MSG_STATUS:
        return "STATUS";
    case USB_RESPONDER_MSG_ERROR:
        return "ERROR";
    case USB_RESPONDER_MSG_FILE_PUT_BEGIN:
        return "FILE_PUT_BEGIN";
    case USB_RESPONDER_MSG_FILE_PUT_CHUNK:
        return "FILE_PUT_CHUNK";
    case USB_RESPONDER_MSG_FILE_PUT_END:
        return "FILE_PUT_END";
    case USB_RESPONDER_MSG_FILE_GET:
        return "FILE_GET";
    case USB_RESPONDER_MSG_FILE_LIST:
        return "FILE_LIST";
    case USB_RESPONDER_MSG_FILE_DELETE:
        return "FILE_DELETE";
    case USB_RESPONDER_MSG_FILE_RENAME:
        return "FILE_RENAME";
    case USB_RESPONDER_MSG_FILE_MKDIR:
        return "FILE_MKDIR";
    case USB_RESPONDER_MSG_FILE_STAT:
        return "FILE_STAT";
    case USB_RESPONDER_MSG_COMMAND_EXEC:
        return "COMMAND_EXEC";
    case USB_RESPONDER_MSG_COMMAND_RESULT:
        return "COMMAND_RESULT";
    case USB_RESPONDER_MSG_DEVINFO:
        return "DEVINFO";
    default:
        return "UNKNOWN";
    }
}

static void log_frame_header(responder_runtime_t* rt, const char* prefix, const usb_responder_frame_header_t* h) {
    if (!rt->cfg.verbose) {
        return;
    }
    fprintf(stderr, "%s type=%s(%u) req=%u payload=%u flags=0x%08x\n",
            prefix,
            msg_type_name(h->type),
            h->type,
            h->request_id,
            h->payload_len,
            h->flags);
}

static bool open_ep(int* out_fd, const char* mount, const char* name, int flags) {
    char path[512];
    int fd = -1;
    if (snprintf(path, sizeof(path), "%s/%s", mount, name) >= (int)sizeof(path)) {
        return false;
    }
    fd = open(path, flags);
    if (fd < 0) {
        perror(path);
        return false;
    }
    *out_fd = fd;
    return true;
}

static void close_eps(responder_runtime_t* rt) {
    if (rt->ep_in_fd >= 0) {
        close(rt->ep_in_fd);
        rt->ep_in_fd = -1;
    }
    if (rt->ep_out_fd >= 0) {
        close(rt->ep_out_fd);
        rt->ep_out_fd = -1;
    }
    rt->rx_size = 0;
    rt->ep_shutdown = false;
}

static bool is_ep_shutdown_errno(int err) {
    return err == ESHUTDOWN || err == ENODEV || err == EPIPE || err == ECONNRESET;
}

static bool rx_reserve(responder_runtime_t* rt, size_t needed) {
    uint8_t* next = NULL;
    size_t new_cap = rt->rx_cap ? rt->rx_cap : 4096u;

    if (needed <= rt->rx_cap) {
        return true;
    }
    while (new_cap < needed) {
        new_cap *= 2u;
    }
    next = (uint8_t*)realloc(rt->rx_buf, new_cap);
    if (!next) {
        usb_responder_set_last_error("rx buffer oom");
        return false;
    }
    rt->rx_buf = next;
    rt->rx_cap = new_cap;
    return true;
}

static bool rx_read_some(responder_runtime_t* rt) {
    uint8_t* tmp = NULL;
    size_t read_size = USB_RESPONDER_IO_CHUNK;
    ssize_t r = 0;

    tmp = (uint8_t*)malloc(read_size);
    if (!tmp) {
        usb_responder_set_last_error("rx read buffer oom");
        return false;
    }
    for (;;) {
        struct pollfd pfd;
        int pr = 0;

        pfd.fd = rt->ep_out_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, USB_RESPONDER_RX_WAIT_LOG_MS);
        if (pr == 0) {
            if (rt->cfg.verbose) {
                fprintf(stderr, "rx waiting for data buffered=%zu\n", rt->rx_size);
            }
            continue;
        }
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "ep_out poll: %s", strerror(errno));
                usb_responder_set_last_error(msg);
            }
            free(tmp);
            return false;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            usb_responder_set_last_error("ep_out poll shutdown");
            rt->ep_shutdown = true;
            free(tmp);
            return false;
        }

        r = read(rt->ep_out_fd, tmp, read_size);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (r <= 0) {
        if (r == 0) {
            usb_responder_set_last_error("ep_out EOF");
        } else {
            char msg[256];
            if (is_ep_shutdown_errno(errno)) {
                rt->ep_shutdown = true;
            }
            snprintf(msg, sizeof(msg), "ep_out read: %s", strerror(errno));
            usb_responder_set_last_error(msg);
        }
        free(tmp);
        return false;
    }
    if (!rx_reserve(rt, rt->rx_size + (size_t)r)) {
        free(tmp);
        return false;
    }
    memcpy(rt->rx_buf + rt->rx_size, tmp, (size_t)r);
    rt->rx_size += (size_t)r;
    free(tmp);
    return true;
}

static bool read_frame_buffered(responder_runtime_t* rt, usb_responder_frame_t* out_frame) {
    uint32_t payload_len = 0;
    size_t frame_size = 0;
    uint8_t* raw = NULL;
    bool ok = false;

    while (rt->rx_size < USB_RESPONDER_HEADER_SIZE) {
        if (!rx_read_some(rt)) {
            return false;
        }
    }
    if (read_le32(rt->rx_buf) != USB_RESPONDER_MAGIC || read_le16(rt->rx_buf + 4) != USB_RESPONDER_VERSION) {
        char msg[256];
        snprintf(msg,
                 sizeof(msg),
                 "bad frame header magic=0x%08x version=%u buffered=%zu",
                 read_le32(rt->rx_buf),
                 read_le16(rt->rx_buf + 4),
                 rt->rx_size);
        usb_responder_set_last_error(msg);
        return false;
    }
    payload_len = read_le32(rt->rx_buf + 16);
    if (payload_len > USB_RESPONDER_MAX_PAYLOAD) {
        usb_responder_set_last_error("payload too large");
        return false;
    }
    frame_size = USB_RESPONDER_HEADER_SIZE + (size_t)payload_len;
    while (rt->rx_size < frame_size) {
        if (!rx_read_some(rt)) {
            return false;
        }
    }

    raw = (uint8_t*)malloc(frame_size);
    if (!raw) {
        usb_responder_set_last_error("frame buffer oom");
        return false;
    }
    memcpy(raw, rt->rx_buf, frame_size);
    if (rt->rx_size > frame_size) {
        memmove(rt->rx_buf, rt->rx_buf + frame_size, rt->rx_size - frame_size);
    }
    rt->rx_size -= frame_size;

    ok = usb_responder_protocol_decode_frame(raw, frame_size, out_frame);
    free(raw);
    if (!ok) {
        usb_responder_set_last_error("decode frame failed");
    } else {
        log_frame_header(rt, "rx frame", &out_frame->header);
    }
    return ok;
}

static bool send_kv_response(
    responder_runtime_t* rt, uint16_t type, uint32_t req_id, const usb_responder_kv_t* kvs, size_t count) {
    usb_responder_frame_t frame;
    uint8_t* payload = NULL;
    size_t payload_size = 0;
    bool ok = false;

    memset(&frame, 0, sizeof(frame));
    if (!usb_responder_protocol_encode_kv(kvs, count, &payload, &payload_size)) {
        return false;
    }
    frame.header.type = type;
    frame.header.request_id = req_id;
    frame.header.payload_len = (uint32_t)payload_size;
    frame.payload = payload;
    log_frame_header(rt, "tx frame begin", &frame.header);
    ok = usb_responder_protocol_write_frame(rt->ep_in_fd, &frame);
    if (ok) {
        log_frame_header(rt, "tx frame done", &frame.header);
    } else if (rt->cfg.verbose) {
        fprintf(stderr, "tx frame failed: %s\n", usb_responder_last_error());
    }
    free(payload);
    return ok;
}

static bool send_error(responder_runtime_t* rt, uint32_t req_id, const char* msg) {
    usb_responder_kv_t kv;
    kv.key = "message";
    kv.value = (char*)msg;
    return send_kv_response(rt, USB_RESPONDER_MSG_ERROR, req_id, &kv, 1);
}

static bool send_last_error(responder_runtime_t* rt, uint32_t req_id, const char* prefix) {
    char msg[512];
    const char* detail = usb_responder_last_error();
    if (detail && detail[0] != '\0') {
        snprintf(msg, sizeof(msg), "%s: %s", prefix, detail);
    } else {
        snprintf(msg, sizeof(msg), "%s", prefix);
    }
    return send_error(rt, req_id, msg);
}

static const char* kv_get(const usb_responder_kv_t* kvs, size_t count, const char* key) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(kvs[i].key, key) == 0) {
            return kvs[i].value;
        }
    }
    return NULL;
}

static bool handle_file_put_begin(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    const char* desire_storage = NULL;
    usb_responder_kv_t status[2];

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    desire_storage = kv_get(kvs, kv_count, "desire_storage");
    if (!path) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "missing path");
    }
    if (!usb_responder_file_begin_upload(&rt->files, in->header.request_id, path, desire_storage)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_last_error(rt, in->header.request_id, "begin upload failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);

    status[0].key = "status";
    status[0].value = "ok";
    status[1].key = "transfer_id";
    status[1].value = "request_id";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, status, 2);
}

static bool handle_file_put_chunk(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    uint32_t transfer_id = in->header.request_id;
    const uint8_t* chunk = in->payload;
    size_t chunk_size = in->header.payload_len;
    usb_responder_kv_t status;

    if (chunk_size >= 4) {
        transfer_id = read_le32(chunk);
        chunk += 4;
        chunk_size -= 4;
    }
    if (!usb_responder_file_append_upload_chunk(&rt->files, transfer_id, chunk, chunk_size)) {
        return send_last_error(rt, in->header.request_id, "append upload failed");
    }
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
}

static bool handle_file_put_end(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    uint32_t transfer_id = in->header.request_id;
    usb_responder_kv_t status;
    if (in->header.payload_len >= 4) {
        transfer_id = read_le32(in->payload);
    }
    if (!usb_responder_file_finish_upload(&rt->files, transfer_id)) {
        return send_last_error(rt, in->header.request_id, "finish upload failed");
    }
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
}

static bool handle_file_get(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    usb_responder_frame_t out;
    size_t payload_size = 0;

    memset(&out, 0, sizeof(out));
    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "missing path");
    }
    if (!usb_responder_file_read(&rt->files, path, &out.payload, &payload_size)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "read file failed");
    }
    if (payload_size > UINT32_MAX) {
        free(out.payload);
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "file too large");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);
    out.header.type = USB_RESPONDER_MSG_FILE_GET;
    out.header.request_id = in->header.request_id;
    out.header.payload_len = (uint32_t)payload_size;
    log_frame_header(rt, "tx frame begin", &out.header);
    if (!usb_responder_protocol_write_frame(rt->ep_in_fd, &out)) {
        if (rt->cfg.verbose) {
            fprintf(stderr, "tx frame failed: %s\n", usb_responder_last_error());
        }
        free(out.payload);
        return false;
    }
    log_frame_header(rt, "tx frame done", &out.header);
    free(out.payload);
    return true;
}

static bool handle_file_list(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    char* file_list = NULL;
    char* dir_list = NULL;
    usb_responder_kv_t status[2];
    bool ok = false;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path) {
        path = ".";
    }
    ok = usb_responder_file_list(&rt->files, path, &file_list, &dir_list);
    usb_responder_protocol_kv_free(kvs, kv_count);
    if (!ok) {
        free(file_list);
        free(dir_list);
        return send_last_error(rt, in->header.request_id, "list failed");
    }
    status[0].key = "files";
    status[0].value = file_list;
    status[1].key = "dirs";
    status[1].value = dir_list;
    ok = send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, status, 2);
    free(file_list);
    free(dir_list);
    return ok;
}

static bool handle_file_delete(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    const char* desire_storage = NULL;
    usb_responder_kv_t status;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    desire_storage = kv_get(kvs, kv_count, "desire_storage");
    if (!path || !usb_responder_file_delete(&rt->files, path, desire_storage)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_last_error(rt, in->header.request_id, "delete failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
}

static bool handle_file_rename(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* from = NULL;
    const char* to = NULL;
    const char* desire_storage = NULL;
    usb_responder_kv_t status;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    from = kv_get(kvs, kv_count, "from");
    to = kv_get(kvs, kv_count, "to");
    desire_storage = kv_get(kvs, kv_count, "desire_storage");
    if (!from || !to || !usb_responder_file_rename(&rt->files, from, to, desire_storage)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_last_error(rt, in->header.request_id, "rename failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
}

static bool handle_file_mkdir(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    const char* parents_val = NULL;
    const char* desire_storage = NULL;
    bool parents = false;
    usb_responder_kv_t status;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    parents_val = kv_get(kvs, kv_count, "parents");
    desire_storage = kv_get(kvs, kv_count, "desire_storage");
    if (parents_val != NULL &&
        (strcmp(parents_val, "1") == 0 || strcasecmp(parents_val, "true") == 0 ||
         strcasecmp(parents_val, "yes") == 0)) {
        parents = true;
    }
    if (!path || !usb_responder_dir_mkdir(&rt->files, path, parents, desire_storage)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_last_error(rt, in->header.request_id, "mkdir failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
}

static bool handle_file_stat(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    usb_responder_stat_info_t st;
    usb_responder_kv_t status[4];

    memset(&st, 0, sizeof(st));
    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path || !usb_responder_file_stat(&rt->files, path, &st)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_last_error(rt, in->header.request_id, "stat failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);

    status[0].key = "owner";
    status[0].value = st.owner;
    status[1].key = "perm";
    status[1].value = st.perm;
    status[2].key = "size";
    status[2].value = st.size;
    status[3].key = "type";
    status[3].value = st.type;
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, status, 4);
}

/* 读取整个文本文件，返回 malloc 的缓冲；max_bytes 为最大允许大小。
 * 出错时 *out 留 NULL，返回 false。 */
static bool read_text_file(const char* path, size_t max_bytes, char** out) {
    FILE* f = NULL;
    char* buf = NULL;
    size_t cap = 1024;
    size_t used = 0;

    *out = NULL;
    f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    buf = (char*)malloc(cap);
    if (!buf) {
        fclose(f);
        return false;
    }
    for (;;) {
        size_t want = 0;
        size_t got = 0;
        if (used + 1 >= cap) {
            size_t new_cap = cap * 2u;
            char* next = NULL;
            if (new_cap > max_bytes + 1) new_cap = max_bytes + 1;
            if (new_cap == cap) break;
            next = (char*)realloc(buf, new_cap);
            if (!next) {
                free(buf);
                fclose(f);
                return false;
            }
            buf = next;
            cap = new_cap;
        }
        want = cap - 1 - used;
        got = fread(buf + used, 1, want, f);
        used += got;
        if (got < want) break;
        if (used >= max_bytes) break;
    }
    fclose(f);
    /* /proc/device-tree/model 这类文件可能含 NUL；统一裁剪到首个 NUL，
     * 然后再 trim 末尾空白。 */
    {
        size_t i = 0;
        while (i < used && buf[i] != '\0') ++i;
        used = i;
    }
    while (used > 0 && (buf[used - 1] == '\n' || buf[used - 1] == '\r' ||
                        buf[used - 1] == ' ' || buf[used - 1] == '\t')) {
        --used;
    }
    buf[used] = '\0';
    *out = buf;
    return true;
}

/* 用我们已有的 shell exec 跑一条短命令，返回 trim 后的 stdout。
 * 失败或非零退出返回 false。 */
static bool exec_capture_trim(const char* cmdline, uint32_t timeout_ms, char** out) {
    usb_responder_shell_exec_options_t opt;
    usb_responder_shell_exec_result_t r;
    char* s = NULL;
    size_t n = 0;

    memset(&opt, 0, sizeof(opt));
    memset(&r, 0, sizeof(r));
    opt.timeout_ms = timeout_ms;
    opt.max_stdout = 4096;
    opt.max_stderr = 4096;

    *out = NULL;
    if (!usb_responder_exec_shell(cmdline, &opt, &r)) {
        return false;
    }
    if (r.timed_out || r.exit_code != 0) {
        usb_responder_shell_exec_result_free(&r);
        return false;
    }
    n = r.stdout_size;
    s = (char*)malloc(n + 1);
    if (!s) {
        usb_responder_shell_exec_result_free(&r);
        return false;
    }
    if (n > 0) memcpy(s, r.stdout_data, n);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        --n;
    }
    s[n] = '\0';
    *out = s;
    usb_responder_shell_exec_result_free(&r);
    return true;
}

static bool handle_devinfo(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[9];
    char* model = NULL;
    char* rootfs = NULL;
    char* app = NULL;
    char kernel_buf[128] = "";
    char sd_mounted_buf[2] = "0";
    char nand_total_buf[32] = "0";
    char nand_free_buf[32] = "0";
    char sd_total_buf[32] = "0";
    char sd_free_buf[32] = "0";
    struct utsname un;
    usb_responder_storage_info_t storage;
    bool ok = false;

    memset(&kvs, 0, sizeof(kvs));

    if (!read_text_file("/proc/device-tree/model", 4096, &model)) {
        model = strdup("");
    }
    if (uname(&un) == 0) {
        snprintf(kernel_buf, sizeof(kernel_buf), "%s", un.release);
    }
    if (!read_text_file("/etc/os-release", 8192, &rootfs)) {
        rootfs = strdup("");
    }
    if (!exec_capture_trim("/root/epass_drm_app version", 3000, &app)) {
        app = strdup("");
    }
    if (usb_responder_storage_info_read(&storage)) {
        snprintf(sd_mounted_buf, sizeof(sd_mounted_buf), "%u", storage.sd_mounted ? 1u : 0u);
        snprintf(nand_total_buf, sizeof(nand_total_buf), "%llu", (unsigned long long)storage.nand_total_bytes);
        snprintf(nand_free_buf, sizeof(nand_free_buf), "%llu", (unsigned long long)storage.nand_free_bytes);
        snprintf(sd_total_buf, sizeof(sd_total_buf), "%llu", (unsigned long long)storage.sd_total_bytes);
        snprintf(sd_free_buf, sizeof(sd_free_buf), "%llu", (unsigned long long)storage.sd_free_bytes);
    }

    kvs[0].key = "model";
    kvs[0].value = model ? model : (char*)"";
    kvs[1].key = "kernel";
    kvs[1].value = kernel_buf;
    kvs[2].key = "rootfs";
    kvs[2].value = rootfs ? rootfs : (char*)"";
    kvs[3].key = "app";
    kvs[3].value = app ? app : (char*)"";
    kvs[4].key = "sd_mounted";
    kvs[4].value = sd_mounted_buf;
    kvs[5].key = "nand_total_bytes";
    kvs[5].value = nand_total_buf;
    kvs[6].key = "nand_free_bytes";
    kvs[6].value = nand_free_buf;
    kvs[7].key = "sd_total_bytes";
    kvs[7].value = sd_total_buf;
    kvs[8].key = "sd_free_bytes";
    kvs[8].value = sd_free_buf;

    ok = send_kv_response(rt, USB_RESPONDER_MSG_DEVINFO, in->header.request_id, kvs, 9);

    free(model);
    free(rootfs);
    free(app);
    return ok;
}

static bool handle_command_exec(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_command_request_t req;
    usb_responder_shell_exec_options_t opt;
    usb_responder_shell_exec_result_t shell;
    usb_responder_command_result_t result;
    usb_responder_frame_t out;
    size_t payload_size = 0;
    bool ok = false;

    memset(&req, 0, sizeof(req));
    memset(&shell, 0, sizeof(shell));
    memset(&result, 0, sizeof(result));
    memset(&out, 0, sizeof(out));

    if (!usb_responder_protocol_decode_command_request(in->payload, in->header.payload_len, &req)) {
        return send_error(rt, in->header.request_id, "invalid command payload");
    }
    opt.timeout_ms = req.timeout_ms ? req.timeout_ms : rt->cfg.default_timeout_ms;
    opt.max_stdout = req.max_stdout ? req.max_stdout : rt->cfg.max_stdout;
    opt.max_stderr = req.max_stderr ? req.max_stderr : rt->cfg.max_stderr;

    if (!usb_responder_exec_shell(req.command, &opt, &shell)) {
        usb_responder_protocol_command_request_free(&req);
        return send_error(rt, in->header.request_id, "shell exec failed");
    }
    usb_responder_protocol_command_request_free(&req);

    result.exit_code = shell.exit_code;
    result.timed_out = shell.timed_out;
    result.duration_ms = shell.duration_ms;
    result.stdout_data = shell.stdout_data;
    result.stdout_size = shell.stdout_size;
    result.stderr_data = shell.stderr_data;
    result.stderr_size = shell.stderr_size;

    if (!usb_responder_protocol_encode_command_result(&result, &out.payload, &payload_size)) {
        usb_responder_shell_exec_result_free(&shell);
        return send_error(rt, in->header.request_id, "encode command result failed");
    }
    if (payload_size > UINT32_MAX) {
        free(out.payload);
        usb_responder_shell_exec_result_free(&shell);
        return send_error(rt, in->header.request_id, "command result too large");
    }
    out.header.type = USB_RESPONDER_MSG_COMMAND_RESULT;
    out.header.request_id = in->header.request_id;
    out.header.payload_len = (uint32_t)payload_size;
    log_frame_header(rt, "tx frame begin", &out.header);
    ok = usb_responder_protocol_write_frame(rt->ep_in_fd, &out);
    if (ok) {
        log_frame_header(rt, "tx frame done", &out.header);
    } else if (rt->cfg.verbose) {
        fprintf(stderr, "tx frame failed: %s\n", usb_responder_last_error());
    }

    free(out.payload);
    usb_responder_shell_exec_result_free(&shell);
    return ok;
}

static bool handle_frame(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t hello[2];

    switch ((usb_responder_msg_type_t)in->header.type) {
    case USB_RESPONDER_MSG_HELLO:
        hello[0].key = "service";
        hello[0].value = "usb_responder";
        hello[1].key = "version";
        hello[1].value = "1";
        return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, hello, 2);
    case USB_RESPONDER_MSG_FILE_PUT_BEGIN:
        return handle_file_put_begin(rt, in);
    case USB_RESPONDER_MSG_FILE_PUT_CHUNK:
        return handle_file_put_chunk(rt, in);
    case USB_RESPONDER_MSG_FILE_PUT_END:
        return handle_file_put_end(rt, in);
    case USB_RESPONDER_MSG_FILE_GET:
        return handle_file_get(rt, in);
    case USB_RESPONDER_MSG_FILE_LIST:
        return handle_file_list(rt, in);
    case USB_RESPONDER_MSG_FILE_DELETE:
        return handle_file_delete(rt, in);
    case USB_RESPONDER_MSG_FILE_RENAME:
        return handle_file_rename(rt, in);
    case USB_RESPONDER_MSG_FILE_MKDIR:
        return handle_file_mkdir(rt, in);
    case USB_RESPONDER_MSG_FILE_STAT:
        return handle_file_stat(rt, in);
    case USB_RESPONDER_MSG_COMMAND_EXEC:
        return handle_command_exec(rt, in);
    case USB_RESPONDER_MSG_DEVINFO:
        return handle_devinfo(rt, in);
    default:
        return send_error(rt, in->header.request_id, "unsupported message type");
    }
}

static bool process_ep0_events(responder_runtime_t* rt, bool* enabled, bool* should_stop) {
    struct usb_functionfs_event events[4];
    ssize_t n = read(rt->ep0_fd, events, sizeof(events));
    size_t count = 0;

    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return true;
        }
        perror("read(ep0)");
        return false;
    }
    if (n % (ssize_t)sizeof(struct usb_functionfs_event) != 0) {
        if (rt->cfg.verbose) {
            fprintf(stderr, "invalid ep0 event size %zd\n", n);
        }
        return false;
    }
    count = (size_t)n / sizeof(struct usb_functionfs_event);
    for (size_t i = 0; i < count; ++i) {
        switch (events[i].type) {
        case FUNCTIONFS_BIND:
            if (rt->cfg.verbose) {
                fprintf(stderr, "ep0: BIND\n");
            }
            break;
        case FUNCTIONFS_ENABLE:
            if (rt->cfg.verbose) {
                fprintf(stderr, "ep0: ENABLE\n");
            }
            *enabled = true;
            break;
        case FUNCTIONFS_DISABLE:
        case FUNCTIONFS_UNBIND:
            if (rt->cfg.verbose) {
                fprintf(stderr, "ep0: DISABLE/UNBIND\n");
            }
            *enabled = false;
            close_eps(rt);
            break;
        case FUNCTIONFS_SETUP:
        case FUNCTIONFS_SUSPEND:
        case FUNCTIONFS_RESUME:
            break;
        default:
            break;
        }
    }
    (void)should_stop;
    return true;
}

static bool handle_io_loop(responder_runtime_t* rt) {
    bool enabled = false;
    bool should_stop = false;

    while (!should_stop) {
        struct pollfd pfds[2];
        nfds_t n = 0;
        int pr = 0;

        pfds[n].fd = rt->ep0_fd;
        pfds[n].events = POLLIN;
        n++;
        if (enabled && rt->ep_out_fd >= 0) {
            pfds[n].fd = rt->ep_out_fd;
            pfds[n].events = POLLIN;
            n++;
        }

        pr = poll(pfds, n, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return false;
        }

        if (pfds[0].revents & POLLIN) {
            if (!process_ep0_events(rt, &enabled, &should_stop)) {
                return false;
            }
            if (enabled && rt->ep_in_fd < 0) {
                if (!open_ep(&rt->ep_in_fd, rt->cfg.ffs_mount, "ep1", O_RDWR) ||
                    !open_ep(&rt->ep_out_fd, rt->cfg.ffs_mount, "ep2", O_RDWR)) {
                    close_eps(rt);
                    enabled = false;
                }
            }
        }

        if (enabled && n > 1 && (pfds[1].revents & POLLIN)) {
            usb_responder_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            if (!read_frame_buffered(rt, &frame)) {
                if (!rt->ep_shutdown && rt->cfg.verbose) {
                    fprintf(stderr, "read frame failed: %s\n", usb_responder_last_error());
                }
                close_eps(rt);
                enabled = false;
                continue;
            }
            log_frame_header(rt, "handle frame begin", &frame.header);
            if (!handle_frame(rt, &frame)) {
                if (rt->cfg.verbose) {
                    fprintf(stderr, "handle frame failed: %s\n", usb_responder_last_error());
                }
            }
            usb_responder_protocol_frame_free(&frame);
        } else if (enabled && n > 1 && (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            close_eps(rt);
            enabled = false;
        }
    }
    return true;
}

bool usb_responder_run(const usb_responder_config_t* config) {
    responder_runtime_t rt;
    uint8_t* desc = NULL;
    size_t desc_size = 0;
    uint8_t* strings = NULL;
    size_t strings_size = 0;
    bool ok = false;

    if (!config || !config->ffs_mount) {
        return false;
    }
    memset(&rt, 0, sizeof(rt));
    rt.cfg = *config;
    rt.ep0_fd = -1;
    rt.ep_in_fd = -1;
    rt.ep_out_fd = -1;
    if (rt.cfg.default_timeout_ms == 0) rt.cfg.default_timeout_ms = 8000;
    if (rt.cfg.max_stdout == 0) rt.cfg.max_stdout = 64 * 1024;
    if (rt.cfg.max_stderr == 0) rt.cfg.max_stderr = 64 * 1024;

    if (!usb_responder_file_ops_init(&rt.files)) {
        return false;
    }
    if (!open_ep(&rt.ep0_fd, rt.cfg.ffs_mount, "ep0", O_RDWR)) {
        goto out;
    }
    if (!usb_responder_build_descriptors_blob(&desc, &desc_size) ||
        !usb_responder_build_strings_blob(&strings, &strings_size)) {
        goto out;
    }
    if (!usb_responder_write_full(rt.ep0_fd, desc, desc_size) ||
        !usb_responder_write_full(rt.ep0_fd, strings, strings_size)) {
        goto out;
    }

    ok = handle_io_loop(&rt);

out:
    close_eps(&rt);
    if (rt.ep0_fd >= 0) {
        close(rt.ep0_fd);
    }
    usb_responder_file_ops_shutdown(&rt.files);
    free(desc);
    free(strings);
    free(rt.rx_buf);
    return ok;
}
