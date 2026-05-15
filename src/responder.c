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
    case USB_RESPONDER_MSG_COMMAND_EXEC:
        return "COMMAND_EXEC";
    case USB_RESPONDER_MSG_COMMAND_RESULT:
        return "COMMAND_RESULT";
    default:
        return "UNKNOWN";
    }
}

static void log_frame_header(const char* prefix, const usb_responder_frame_header_t* h) {
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
            fprintf(stderr, "rx waiting for data buffered=%zu\n", rt->rx_size);
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
        log_frame_header("rx frame", &out_frame->header);
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
    log_frame_header("tx frame begin", &frame.header);
    ok = usb_responder_protocol_write_frame(rt->ep_in_fd, &frame);
    if (ok) {
        log_frame_header("tx frame done", &frame.header);
    } else {
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
    usb_responder_kv_t status[2];

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "missing path");
    }
    if (!usb_responder_file_begin_upload(&rt->files, in->header.request_id, path)) {
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
    log_frame_header("tx frame begin", &out.header);
    if (!usb_responder_protocol_write_frame(rt->ep_in_fd, &out)) {
        fprintf(stderr, "tx frame failed: %s\n", usb_responder_last_error());
        free(out.payload);
        return false;
    }
    log_frame_header("tx frame done", &out.header);
    free(out.payload);
    return true;
}

static bool handle_file_list(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    char* listing = NULL;
    usb_responder_kv_t status[2];
    bool ok = false;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path) {
        path = ".";
    }
    ok = usb_responder_file_list(&rt->files, path, &listing);
    usb_responder_protocol_kv_free(kvs, kv_count);
    if (!ok) {
        return send_error(rt, in->header.request_id, "list failed");
    }
    status[0].key = "status";
    status[0].value = "ok";
    status[1].key = "entries";
    status[1].value = listing;
    ok = send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, status, 2);
    free(listing);
    return ok;
}

static bool handle_file_delete(responder_runtime_t* rt, const usb_responder_frame_t* in) {
    usb_responder_kv_t kvs[USB_RESPONDER_MAX_KV] = {0};
    size_t kv_count = 0;
    const char* path = NULL;
    usb_responder_kv_t status;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    path = kv_get(kvs, kv_count, "path");
    if (!path || !usb_responder_file_delete(&rt->files, path)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "delete failed");
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
    usb_responder_kv_t status;

    if (!usb_responder_protocol_decode_kv(in->payload, in->header.payload_len, kvs, USB_RESPONDER_MAX_KV, &kv_count)) {
        return send_error(rt, in->header.request_id, "invalid kv payload");
    }
    from = kv_get(kvs, kv_count, "from");
    to = kv_get(kvs, kv_count, "to");
    if (!from || !to || !usb_responder_file_rename(&rt->files, from, to)) {
        usb_responder_protocol_kv_free(kvs, kv_count);
        return send_error(rt, in->header.request_id, "rename failed");
    }
    usb_responder_protocol_kv_free(kvs, kv_count);
    status.key = "status";
    status.value = "ok";
    return send_kv_response(rt, USB_RESPONDER_MSG_STATUS, in->header.request_id, &status, 1);
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
    log_frame_header("tx frame begin", &out.header);
    ok = usb_responder_protocol_write_frame(rt->ep_in_fd, &out);
    if (ok) {
        log_frame_header("tx frame done", &out.header);
    } else {
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
    case USB_RESPONDER_MSG_COMMAND_EXEC:
        return handle_command_exec(rt, in);
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
        fprintf(stderr, "invalid ep0 event size %zd\n", n);
        return false;
    }
    count = (size_t)n / sizeof(struct usb_functionfs_event);
    for (size_t i = 0; i < count; ++i) {
        switch (events[i].type) {
        case FUNCTIONFS_BIND:
            fprintf(stderr, "ep0: BIND\n");
            break;
        case FUNCTIONFS_ENABLE:
            fprintf(stderr, "ep0: ENABLE\n");
            *enabled = true;
            break;
        case FUNCTIONFS_DISABLE:
        case FUNCTIONFS_UNBIND:
            fprintf(stderr, "ep0: DISABLE/UNBIND\n");
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
                if (!rt->ep_shutdown) {
                    fprintf(stderr, "read frame failed: %s\n", usb_responder_last_error());
                }
                close_eps(rt);
                enabled = false;
                continue;
            }
            log_frame_header("handle frame begin", &frame.header);
            if (!handle_frame(rt, &frame)) {
                fprintf(stderr, "handle frame failed: %s\n", usb_responder_last_error());
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

    if (!config || !config->ffs_mount || !config->media_root) {
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

    if (!usb_responder_file_ops_init(&rt.files, rt.cfg.media_root)) {
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
