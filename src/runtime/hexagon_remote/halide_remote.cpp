extern "C" {

#include "bin/src/halide_hexagon_remote.h"

typedef halide_hexagon_remote_handle_t handle_t;
typedef halide_hexagon_remote_buffer buffer;

int halide_hexagon_remote_initialize_kernels(const unsigned char *code, int codeLen,
                                             handle_t *module_ptr) {
    return 0;
}

handle_t halide_hexagon_remote_get_symbol(handle_t module_ptr, const char* name, int nameLen) {
    // Can't return 0, that is an error.
    return 1;
}

int halide_hexagon_remote_run(handle_t module_ptr, handle_t function,
                              const buffer *input_buffersPtrs, int input_buffersLen,
                              buffer *output_buffersPtrs, int output_buffersLen,
                              const buffer *input_scalarsPtrs, int input_scalarsLen) {
    return 0;
}

int halide_hexagon_remote_poll_log(char *out, int size, int *read_size) {
    *read_size = 0;
    return 0;
}

int halide_hexagon_remote_release_kernels(handle_t module_ptr, int codeLen) {
    return 0;
}

}  // extern "C"
