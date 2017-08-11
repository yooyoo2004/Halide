# Generator
halide_generator(local_laplacian.generator
                     SRCS local_laplacian_generator.cpp)
halide_library_from_generator(local_laplacian
                       GENERATOR local_laplacian.generator
                       GENERATOR_ARGS target=host)

# Final executable
add_executable(ll_process process.cpp)
target_link_libraries(ll_process PUBLIC local_laplacian)
use_image_io(ll_process)
