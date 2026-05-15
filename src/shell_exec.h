#ifndef USB_RESPONDER_SHELL_EXEC_H
#define USB_RESPONDER_SHELL_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct usb_responder_shell_exec_options {
    uint32_t timeout_ms;
    uint32_t max_stdout;
    uint32_t max_stderr;
} usb_responder_shell_exec_options_t;

typedef struct usb_responder_shell_exec_result {
    int32_t exit_code;
    bool timed_out;
    uint32_t duration_ms;
    uint8_t* stdout_data;
    size_t stdout_size;
    uint8_t* stderr_data;
    size_t stderr_size;
} usb_responder_shell_exec_result_t;

bool usb_responder_exec_shell(
    const char* command,
    const usb_responder_shell_exec_options_t* options,
    usb_responder_shell_exec_result_t* out_result);
void usb_responder_shell_exec_result_free(usb_responder_shell_exec_result_t* result);

#endif
