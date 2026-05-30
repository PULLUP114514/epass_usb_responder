#include "protocol.h"

#include "crc32.h"
#include "io_utils.h"

#include <stdlib.h>
#include <string.h>

#define USB_RESPONDER_IO_CHUNK (16u * 1024u)

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static bool append_bytes(uint8_t** buf, size_t* used, size_t* cap, const void* src, size_t n) {
    uint8_t* next = NULL;
    if (*used + n > *cap) {
        size_t new_cap = (*cap == 0) ? 64u : *cap;
        while (new_cap < *used + n) {
            new_cap *= 2u;
        }
        next = (uint8_t*)realloc(*buf, new_cap);
        if (!next) {
            return false;
        }
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *used, src, n);
    *used += n;
    return true;
}

static bool write_limited(int fd, const uint8_t* data, size_t size) {
    size_t off = 0;
    while (off < size) {
        size_t n = size - off;
        if (n > USB_RESPONDER_IO_CHUNK) {
            n = USB_RESPONDER_IO_CHUNK;
        }
        if (!usb_responder_write_full(fd, data + off, n)) {
            return false;
        }
        off += n;
    }
    /* ZLP 只在整帧写完后按总帧长补一次，绝不在分片之间插入。 */
    if (usb_responder_bulk_in_needs_zlp(size)) {
        return usb_responder_write_zlp(fd);
    }
    return true;
}

bool usb_responder_protocol_encode_frame(
    const usb_responder_frame_t* frame, uint8_t** out_raw, size_t* out_size) {
    uint8_t* raw = NULL;
    uint32_t payload_crc = 0;

    if (!frame || !out_raw || !out_size) {
        return false;
    }
    if (frame->header.payload_len > 0 && !frame->payload) {
        return false;
    }

    raw = (uint8_t*)malloc(USB_RESPONDER_HEADER_SIZE + frame->header.payload_len);
    if (!raw) {
        return false;
    }
    payload_crc = usb_responder_crc32(frame->payload, frame->header.payload_len);

    write_le32(raw, USB_RESPONDER_MAGIC);
    write_le16(raw + 4, USB_RESPONDER_VERSION);
    write_le16(raw + 6, frame->header.type);
    write_le32(raw + 8, frame->header.flags);
    write_le32(raw + 12, frame->header.request_id);
    write_le32(raw + 16, frame->header.payload_len);
    write_le32(raw + 20, payload_crc);
    if (frame->header.payload_len > 0) {
        memcpy(raw + USB_RESPONDER_HEADER_SIZE, frame->payload, frame->header.payload_len);
    }

    *out_raw = raw;
    *out_size = USB_RESPONDER_HEADER_SIZE + frame->header.payload_len;
    return true;
}

bool usb_responder_protocol_decode_frame(
    const uint8_t* raw, size_t raw_size, usb_responder_frame_t* out_frame) {
    uint32_t payload_len = 0;
    uint32_t crc_expect = 0;
    uint32_t crc_real = 0;

    if (!raw || !out_frame || raw_size < USB_RESPONDER_HEADER_SIZE) {
        return false;
    }
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->header.magic = read_le32(raw);
    out_frame->header.version = read_le16(raw + 4);
    out_frame->header.type = read_le16(raw + 6);
    out_frame->header.flags = read_le32(raw + 8);
    out_frame->header.request_id = read_le32(raw + 12);
    out_frame->header.payload_len = read_le32(raw + 16);
    out_frame->header.payload_crc32 = read_le32(raw + 20);

    if (out_frame->header.magic != USB_RESPONDER_MAGIC ||
        out_frame->header.version != USB_RESPONDER_VERSION) {
        return false;
    }

    payload_len = out_frame->header.payload_len;
    if (USB_RESPONDER_HEADER_SIZE + payload_len != raw_size) {
        return false;
    }

    if (payload_len > 0) {
        out_frame->payload = (uint8_t*)malloc(payload_len);
        if (!out_frame->payload) {
            return false;
        }
        memcpy(out_frame->payload, raw + USB_RESPONDER_HEADER_SIZE, payload_len);
    }

    crc_expect = out_frame->header.payload_crc32;
    crc_real = usb_responder_crc32(out_frame->payload, payload_len);
    if (crc_expect != crc_real) {
        usb_responder_protocol_frame_free(out_frame);
        return false;
    }
    return true;
}

bool usb_responder_protocol_read_frame(
    int fd, size_t max_payload, usb_responder_frame_t* out_frame) {
    uint8_t hdr[USB_RESPONDER_HEADER_SIZE];
    uint32_t payload_len = 0;
    uint8_t* raw = NULL;
    bool ok = false;

    if (!out_frame) {
        return false;
    }
    if (!usb_responder_read_full(fd, hdr, sizeof(hdr))) {
        return false;
    }
    payload_len = read_le32(hdr + 16);
    if (payload_len > max_payload) {
        return false;
    }
    raw = (uint8_t*)malloc(USB_RESPONDER_HEADER_SIZE + payload_len);
    if (!raw) {
        return false;
    }
    memcpy(raw, hdr, USB_RESPONDER_HEADER_SIZE);
    if (payload_len > 0 && !usb_responder_read_full(fd, raw + USB_RESPONDER_HEADER_SIZE, payload_len)) {
        free(raw);
        return false;
    }
    ok = usb_responder_protocol_decode_frame(raw, USB_RESPONDER_HEADER_SIZE + payload_len, out_frame);
    free(raw);
    return ok;
}

bool usb_responder_protocol_write_frame(int fd, const usb_responder_frame_t* frame) {
    uint8_t* raw = NULL;
    size_t raw_size = 0;
    bool ok = false;

    if (!usb_responder_protocol_encode_frame(frame, &raw, &raw_size)) {
        return false;
    }
    ok = write_limited(fd, raw, raw_size);
    free(raw);
    return ok;
}

void usb_responder_protocol_frame_free(usb_responder_frame_t* frame) {
    if (!frame) {
        return;
    }
    free(frame->payload);
    frame->payload = NULL;
    memset(&frame->header, 0, sizeof(frame->header));
}

bool usb_responder_protocol_encode_kv(
    const usb_responder_kv_t* items, size_t count, uint8_t** out_data, size_t* out_size) {
    uint8_t* buf = NULL;
    size_t used = 0;
    size_t cap = 0;
    uint8_t nbuf[2];

    if (!out_data || !out_size || count > USB_RESPONDER_MAX_KV) {
        return false;
    }
    write_le16(nbuf, (uint16_t)count);
    if (!append_bytes(&buf, &used, &cap, nbuf, sizeof(nbuf))) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        uint16_t klen = (uint16_t)strlen(items[i].key ? items[i].key : "");
        uint16_t vlen = (uint16_t)strlen(items[i].value ? items[i].value : "");
        uint8_t lens[4];
        write_le16(lens, klen);
        write_le16(lens + 2, vlen);
        if (!append_bytes(&buf, &used, &cap, lens, sizeof(lens))) {
            free(buf);
            return false;
        }
        if (klen > 0 && !append_bytes(&buf, &used, &cap, items[i].key, klen)) {
            free(buf);
            return false;
        }
        if (vlen > 0 && !append_bytes(&buf, &used, &cap, items[i].value, vlen)) {
            free(buf);
            return false;
        }
    }
    *out_data = buf;
    *out_size = used;
    return true;
}

bool usb_responder_protocol_decode_kv(
    const uint8_t* data, size_t size, usb_responder_kv_t* out_items, size_t max_items, size_t* out_count) {
    size_t off = 0;
    uint16_t count = 0;

    if (!data || !out_items || !out_count || size < 2) {
        return false;
    }
    count = read_le16(data);
    off = 2;
    if (count > max_items || count > USB_RESPONDER_MAX_KV) {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t klen = 0;
        uint16_t vlen = 0;
        if (off + 4 > size) {
            usb_responder_protocol_kv_free(out_items, i);
            return false;
        }
        klen = read_le16(data + off);
        vlen = read_le16(data + off + 2);
        off += 4;
        if (off + (size_t)klen + (size_t)vlen > size) {
            usb_responder_protocol_kv_free(out_items, i);
            return false;
        }
        out_items[i].key = (char*)malloc((size_t)klen + 1);
        out_items[i].value = (char*)malloc((size_t)vlen + 1);
        if (!out_items[i].key || !out_items[i].value) {
            usb_responder_protocol_kv_free(out_items, i + 1);
            return false;
        }
        memcpy(out_items[i].key, data + off, klen);
        out_items[i].key[klen] = '\0';
        off += klen;
        memcpy(out_items[i].value, data + off, vlen);
        out_items[i].value[vlen] = '\0';
        off += vlen;
    }
    *out_count = count;
    return off == size;
}

void usb_responder_protocol_kv_free(usb_responder_kv_t* items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(items[i].key);
        free(items[i].value);
        items[i].key = NULL;
        items[i].value = NULL;
    }
}

bool usb_responder_protocol_encode_command_request(
    const usb_responder_command_request_t* req, uint8_t** out_data, size_t* out_size) {
    size_t cmd_len = 0;
    uint8_t* out = NULL;

    if (!req || !req->command || !out_data || !out_size) {
        return false;
    }
    cmd_len = strlen(req->command);
    out = (uint8_t*)malloc(16 + cmd_len);
    if (!out) {
        return false;
    }
    write_le32(out, req->timeout_ms);
    write_le32(out + 4, req->max_stdout);
    write_le32(out + 8, req->max_stderr);
    write_le32(out + 12, (uint32_t)cmd_len);
    if (cmd_len > 0) {
        memcpy(out + 16, req->command, cmd_len);
    }
    *out_data = out;
    *out_size = 16 + cmd_len;
    return true;
}

bool usb_responder_protocol_decode_command_request(
    const uint8_t* payload, size_t payload_size, usb_responder_command_request_t* out_req) {
    uint32_t cmd_len = 0;

    if (!payload || !out_req || payload_size < 16) {
        return false;
    }
    memset(out_req, 0, sizeof(*out_req));
    out_req->timeout_ms = read_le32(payload);
    out_req->max_stdout = read_le32(payload + 4);
    out_req->max_stderr = read_le32(payload + 8);
    cmd_len = read_le32(payload + 12);
    if (16u + cmd_len != payload_size) {
        return false;
    }
    out_req->command = (char*)malloc((size_t)cmd_len + 1);
    if (!out_req->command) {
        return false;
    }
    memcpy(out_req->command, payload + 16, cmd_len);
    out_req->command[cmd_len] = '\0';
    return true;
}

void usb_responder_protocol_command_request_free(usb_responder_command_request_t* req) {
    if (!req) {
        return;
    }
    free(req->command);
    req->command = NULL;
}

bool usb_responder_protocol_encode_command_result(
    const usb_responder_command_result_t* result, uint8_t** out_data, size_t* out_size) {
    uint8_t* out = NULL;
    size_t total = 0;

    if (!result || !out_data || !out_size) {
        return false;
    }
    total = 20 + result->stdout_size + result->stderr_size;
    out = (uint8_t*)malloc(total);
    if (!out) {
        return false;
    }
    write_le32(out, (uint32_t)result->exit_code);
    out[4] = result->timed_out ? 1u : 0u;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    write_le32(out + 8, result->duration_ms);
    write_le32(out + 12, (uint32_t)result->stdout_size);
    write_le32(out + 16, (uint32_t)result->stderr_size);
    if (result->stdout_size > 0) {
        memcpy(out + 20, result->stdout_data, result->stdout_size);
    }
    if (result->stderr_size > 0) {
        memcpy(out + 20 + result->stdout_size, result->stderr_data, result->stderr_size);
    }
    *out_data = out;
    *out_size = total;
    return true;
}

bool usb_responder_protocol_decode_command_result(
    const uint8_t* payload, size_t payload_size, usb_responder_command_result_t* out_result) {
    uint32_t stdout_len = 0;
    uint32_t stderr_len = 0;
    size_t total = 0;

    if (!payload || !out_result || payload_size < 20) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->exit_code = (int32_t)read_le32(payload);
    out_result->timed_out = payload[4] != 0;
    out_result->duration_ms = read_le32(payload + 8);
    stdout_len = read_le32(payload + 12);
    stderr_len = read_le32(payload + 16);
    total = 20u + (size_t)stdout_len + (size_t)stderr_len;
    if (total != payload_size) {
        return false;
    }

    if (stdout_len > 0) {
        out_result->stdout_data = (uint8_t*)malloc(stdout_len);
        if (!out_result->stdout_data) {
            return false;
        }
        memcpy(out_result->stdout_data, payload + 20, stdout_len);
        out_result->stdout_size = stdout_len;
    }
    if (stderr_len > 0) {
        out_result->stderr_data = (uint8_t*)malloc(stderr_len);
        if (!out_result->stderr_data) {
            usb_responder_protocol_command_result_free(out_result);
            return false;
        }
        memcpy(out_result->stderr_data, payload + 20 + stdout_len, stderr_len);
        out_result->stderr_size = stderr_len;
    }
    return true;
}

void usb_responder_protocol_command_result_free(usb_responder_command_result_t* result) {
    if (!result) {
        return;
    }
    free(result->stdout_data);
    free(result->stderr_data);
    memset(result, 0, sizeof(*result));
}
