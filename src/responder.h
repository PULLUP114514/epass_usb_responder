#ifndef USB_RESPONDER_RESPONDER_H
#define USB_RESPONDER_RESPONDER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct usb_responder_config {
    const char* ffs_mount;
    const char* media_root;
    uint32_t default_timeout_ms;
    uint32_t max_stdout;
    uint32_t max_stderr;
} usb_responder_config_t;

bool usb_responder_run(const usb_responder_config_t* config);

#endif
