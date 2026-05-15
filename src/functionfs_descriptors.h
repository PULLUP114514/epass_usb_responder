#ifndef USB_RESPONDER_FUNCTIONFS_DESCRIPTORS_H
#define USB_RESPONDER_FUNCTIONFS_DESCRIPTORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool usb_responder_build_descriptors_blob(uint8_t** out_data, size_t* out_size);
bool usb_responder_build_strings_blob(uint8_t** out_data, size_t* out_size);

#endif
