#ifndef USB_RESPONDER_IO_UTILS_H
#define USB_RESPONDER_IO_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool usb_responder_read_full(int fd, uint8_t* out, size_t size);
bool usb_responder_write_full(int fd, const uint8_t* data, size_t size);
bool usb_responder_bulk_in_needs_zlp(size_t nbytes);
bool usb_responder_write_zlp(int fd);
bool usb_responder_read_file_all(const char* path, uint8_t** out_data, size_t* out_size);
void usb_responder_free(void* ptr);
void usb_responder_set_last_error(const char* msg);
const char* usb_responder_last_error(void);

#endif
