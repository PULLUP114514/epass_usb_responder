#ifndef USB_RESPONDER_FILE_OPS_H
#define USB_RESPONDER_FILE_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define USB_RESPONDER_MAX_UPLOAD_SESSIONS 16u
#define USB_RESPONDER_PATH_MAX_LEN 4096u

typedef struct usb_responder_upload_session {
    bool used;
    uint32_t transfer_id;
    FILE* fp;
    char relative_path[USB_RESPONDER_PATH_MAX_LEN];
    char temp_path[USB_RESPONDER_PATH_MAX_LEN];
    char final_path[USB_RESPONDER_PATH_MAX_LEN];
    uint64_t bytes_written;
    uint64_t synced_bytes;
    bool apply_perm;
    mode_t file_mode;
} usb_responder_upload_session_t;

typedef enum usb_responder_storage {
    USB_RESPONDER_STORAGE_NAND = 0,
    USB_RESPONDER_STORAGE_SD = 1,
} usb_responder_storage_t;

typedef struct usb_responder_storage_info {
    bool sd_mounted;
    uint64_t nand_total_bytes;
    uint64_t nand_free_bytes;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;
} usb_responder_storage_info_t;

typedef struct usb_responder_file_ops {
    usb_responder_upload_session_t sessions[USB_RESPONDER_MAX_UPLOAD_SESSIONS];
} usb_responder_file_ops_t;

bool usb_responder_file_ops_init(usb_responder_file_ops_t* ops);
void usb_responder_file_ops_shutdown(usb_responder_file_ops_t* ops);

const char* usb_responder_storage_name(usb_responder_storage_t storage);
bool usb_responder_storage_from_name(const char* name, usb_responder_storage_t* out);
bool usb_responder_storage_info_read(usb_responder_storage_info_t* out);

bool usb_responder_file_begin_upload(
    usb_responder_file_ops_t* ops,
    uint32_t transfer_id,
    const char* relative_path,
    const char* desire_storage,
    const char* perm);
bool usb_responder_file_append_upload_chunk(
    usb_responder_file_ops_t* ops, uint32_t transfer_id, const uint8_t* data, size_t size);
bool usb_responder_file_finish_upload(usb_responder_file_ops_t* ops, uint32_t transfer_id);
bool usb_responder_file_abort_upload(usb_responder_file_ops_t* ops, uint32_t transfer_id);

bool usb_responder_file_read(
    const usb_responder_file_ops_t* ops, const char* relative_path, uint8_t** out_data, size_t* out_size);
bool usb_responder_file_list(
    const usb_responder_file_ops_t* ops, const char* relative_path, char** out_files, char** out_dirs);

typedef struct usb_responder_stat_info {
    char owner[256];
    char perm[16];
    char size[32];
    char type[16];
} usb_responder_stat_info_t;

bool usb_responder_file_stat(
    const usb_responder_file_ops_t* ops, const char* relative_path, usb_responder_stat_info_t* out);

bool usb_responder_file_delete(
    const usb_responder_file_ops_t* ops, const char* relative_path, const char* desire_storage);
bool usb_responder_dir_mkdir(
    usb_responder_file_ops_t* ops, const char* relative_path, bool parents, const char* desire_storage);
bool usb_responder_file_rename(
    const usb_responder_file_ops_t* ops,
    const char* from_relative,
    const char* to_relative,
    const char* desire_storage);

#endif
