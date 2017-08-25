# Generator
halide_generator(conv_layer.generator
                     SRCS conv_layer_generator.cpp)
halide_library_from_generator(conv_layer
                       GENERATOR conv_layer.generator
                       GENERATOR_ARGS target=host auto_schedule=false)
halide_library_from_generator(conv_layer_auto_schedule
                       GENERATOR conv_layer.generator
                       GENERATOR_ARGS target=host auto_schedule=true)

# Final executable
add_executable(conv_layer_process process.cpp)
target_link_libraries(conv_layer_process PUBLIC conv_layer conv_layer_auto_schedule)
halide_use_image_io(conv_layer_process)
