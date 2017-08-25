# Generator
halide_generator(lens_blur.generator
                     SRCS lens_blur_generator.cpp)
halide_library_from_generator(lens_blur
                       GENERATOR lens_blur.generator
                       GENERATOR_ARGS target=host auto_schedule=false)
halide_library_from_generator(lens_blur_auto_schedule
                       GENERATOR lens_blur.generator
                       GENERATOR_ARGS target=host auto_schedule=true)

# Final executable
add_executable(lensblur_process process.cpp)
target_link_libraries(lensblur_process PUBLIC lens_blur lens_blur_auto_schedule)
halide_use_image_io(lensblur_process)
