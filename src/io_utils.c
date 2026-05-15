#include "io_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_last_error[256];

void usb_responder_set_last_error(const char* msg) {
    if (!msg) {
        g_last_error[0] = '\0';
        return;
    }
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

static void set_errno_error(const char* prefix) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s", prefix, strerror(errno));
    usb_responder_set_last_error(buf);
}

bool usb_responder_read_full(int fd, uint8_t* out, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t r = read(fd, out + done, size - done);
        if (r == 0) {
        usb_responder_set_last_error("unexpected EOF");
            return false;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_errno_error("read");
            return false;
        }
        done += (size_t)r;
    }
    return true;
}

bool usb_responder_write_full(int fd, const uint8_t* data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t w = write(fd, data + done, size - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_errno_error("write");
            return false;
        }
        if (w == 0) {
            usb_responder_set_last_error("write returned 0");
            return false;
        }
        done += (size_t)w;
    }
    return true;
}

bool usb_responder_read_file_all(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* f = NULL;
    uint8_t* data = NULL;
    long file_size = 0;

    if (!path || !out_data || !out_size) {
        usb_responder_set_last_error("invalid arguments");
        return false;
    }

    *out_data = NULL;
    *out_size = 0;

    f = fopen(path, "rb");
    if (!f) {
        set_errno_error("fopen");
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_errno_error("fseek");
        fclose(f);
        return false;
    }
    file_size = ftell(f);
    if (file_size < 0) {
        set_errno_error("ftell");
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        set_errno_error("fseek");
        fclose(f);
        return false;
    }

    data = (uint8_t*)malloc((size_t)file_size);
    if (file_size > 0 && !data) {
        usb_responder_set_last_error("oom");
        fclose(f);
        return false;
    }

    if (file_size > 0 && fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
        set_errno_error("fread");
        fclose(f);
        free(data);
        return false;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)file_size;
    return true;
}

void usb_responder_free(void* ptr) {
    free(ptr);
}

const char* usb_responder_last_error(void) {
    return g_last_error;
}
