#ifndef USB_RESPONDER_PROTOCOL_H
#define USB_RESPONDER_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_RESPONDER_MAGIC 0x45504153u
#define USB_RESPONDER_VERSION 1u
#define USB_RESPONDER_HEADER_SIZE 24u
#define USB_RESPONDER_MAX_KV 32u

typedef enum usb_responder_msg_type {
    USB_RESPONDER_MSG_HELLO = 1,
    USB_RESPONDER_MSG_STATUS = 2,
    USB_RESPONDER_MSG_ERROR = 3,
    USB_RESPONDER_MSG_FILE_PUT_BEGIN = 10,
    USB_RESPONDER_MSG_FILE_PUT_CHUNK = 11,
    USB_RESPONDER_MSG_FILE_PUT_END = 12,
    USB_RESPONDER_MSG_FILE_GET = 13,
    USB_RESPONDER_MSG_FILE_LIST = 14,
    USB_RESPONDER_MSG_FILE_DELETE = 15,
    USB_RESPONDER_MSG_FILE_RENAME = 16,
    USB_RESPONDER_MSG_FILE_MKDIR = 17,
    USB_RESPONDER_MSG_COMMAND_EXEC = 20,
    USB_RESPONDER_MSG_COMMAND_RESULT = 21,
    USB_RESPONDER_MSG_DEVINFO = 30,
} usb_responder_msg_type_t;

typedef struct usb_responder_frame_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint32_t request_id;
    uint32_t payload_len;
    uint32_t payload_crc32;
} usb_responder_frame_header_t;

typedef struct usb_responder_frame {
    usb_responder_frame_header_t header;
    uint8_t* payload;
} usb_responder_frame_t;

typedef struct usb_responder_kv {
    char* key;
    char* value;
} usb_responder_kv_t;

typedef struct usb_responder_command_request {
    uint32_t timeout_ms;
    uint32_t max_stdout;
    uint32_t max_stderr;
    char* command;
} usb_responder_command_request_t;

typedef struct usb_responder_command_result {
    int32_t exit_code;
    bool timed_out;
    uint32_t duration_ms;
    uint8_t* stdout_data;
    size_t stdout_size;
    uint8_t* stderr_data;
    size_t stderr_size;
} usb_responder_command_result_t;

bool usb_responder_protocol_encode_frame(
    const usb_responder_frame_t* frame, uint8_t** out_raw, size_t* out_size);
bool usb_responder_protocol_decode_frame(
    const uint8_t* raw, size_t raw_size, usb_responder_frame_t* out_frame);
bool usb_responder_protocol_read_frame(
    int fd, size_t max_payload, usb_responder_frame_t* out_frame);
bool usb_responder_protocol_write_frame(int fd, const usb_responder_frame_t* frame);
void usb_responder_protocol_frame_free(usb_responder_frame_t* frame);

bool usb_responder_protocol_encode_kv(
    const usb_responder_kv_t* items, size_t count, uint8_t** out_data, size_t* out_size);
bool usb_responder_protocol_decode_kv(
    const uint8_t* data, size_t size, usb_responder_kv_t* out_items, size_t max_items, size_t* out_count);
void usb_responder_protocol_kv_free(usb_responder_kv_t* items, size_t count);

bool usb_responder_protocol_encode_command_request(
    const usb_responder_command_request_t* req, uint8_t** out_data, size_t* out_size);
bool usb_responder_protocol_decode_command_request(
    const uint8_t* payload, size_t payload_size, usb_responder_command_request_t* out_req);
void usb_responder_protocol_command_request_free(usb_responder_command_request_t* req);

bool usb_responder_protocol_encode_command_result(
    const usb_responder_command_result_t* result, uint8_t** out_data, size_t* out_size);
bool usb_responder_protocol_decode_command_result(
    const uint8_t* payload, size_t payload_size, usb_responder_command_result_t* out_result);
void usb_responder_protocol_command_result_free(usb_responder_command_result_t* result);

#endif
