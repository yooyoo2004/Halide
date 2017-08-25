# Generator
halide_generator(nl_means.generator
                     SRCS nl_means_generator.cpp)
halide_library_from_generator(nl_means
                       GENERATOR nl_means.generator
                       GENERATOR_ARGS target=host auto_schedule=false)
halide_library_from_generator(nl_means_auto_schedule
                       GENERATOR nl_means.generator
                       GENERATOR_ARGS target=host auto_schedule=true)

# Final executable
add_executable(nl_means_process process.cpp)
target_link_libraries(nl_means_process PUBLIC nl_means nl_means_auto_schedule)
halide_use_image_io(nl_means_process)
