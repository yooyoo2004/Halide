# Generator
halide_generator(local_laplacian.generator
                     SRCS local_laplacian_generator.cpp)
halide_library_from_generator(local_laplacian
                       GENERATOR local_laplacian.generator
                       GENERATOR_ARGS target=host auto_schedule=false)
halide_library_from_generator(local_laplacian_auto_schedule
                       GENERATOR local_laplacian.generator
                       GENERATOR_ARGS target=host auto_schedule=true)

# Final executable
add_executable(ll_process process.cpp)
target_link_libraries(ll_process PUBLIC local_laplacian local_laplacian_auto_schedule)
halide_use_image_io(ll_process)
