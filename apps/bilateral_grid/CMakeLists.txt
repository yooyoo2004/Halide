# Generator
halide_generator(bilateral_grid.generator
                     SRCS bilateral_grid_generator.cpp)
halide_library_from_generator(bilateral_grid
                       GENERATOR bilateral_grid.generator
                       GENERATOR_ARGS target=host)

# Final executable
add_executable(filter filter.cpp)
target_link_libraries(filter PUBLIC bilateral_grid)
use_image_io(filter)
