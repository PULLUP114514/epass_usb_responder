cmake_minimum_required(VERSION 3.16)
project(epass_usb_responder VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)


add_executable(usb_responder 
    src/main.c
    src/crc32.c
    src/file_ops.c
    src/functionfs_descriptors.c
    src/io_utils.c
    src/protocol.c
    src/responder.c
    src/shell_exec.c
)

target_include_directories(usb_responder
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(usb_responder PRIVATE rt)

target_compile_options(usb_responder
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
target_compile_definitions(usb_responder PRIVATE _GNU_SOURCE)

