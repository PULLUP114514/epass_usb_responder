#include "responder.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "Usage: %s --ffs <mount_dir> --media-root <dir> "
            "[-v|--verbose] [--timeout-ms N] [--max-stdout N] [--max-stderr N]\n",
            argv0);
}

int main(int argc, char** argv) {
    usb_responder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ffs") == 0 && i + 1 < argc) {
            cfg.ffs_mount = argv[++i];
        } else if (strcmp(argv[i], "--media-root") == 0 && i + 1 < argc) {
            cfg.media_root = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            cfg.default_timeout_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-stdout") == 0 && i + 1 < argc) {
            cfg.max_stdout = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-stderr") == 0 && i + 1 < argc) {
            cfg.max_stderr = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (!cfg.ffs_mount || !cfg.media_root) {
        print_usage(argv[0]);
        return 1;
    }

    if (!usb_responder_run(&cfg)) {
        fprintf(stderr, "epass_usb_responder failed\n");
        return 2;
    }
    return 0;
}
