# Generators
find_package(OpenGL)
if (OpenGL_FOUND)

    # Generators
    halide_generator(halide_blur_glsl.generator
                         SRCS halide_blur_glsl_generator.cpp)
    halide_library_from_generator(halide_blur_glsl
                           GENERATOR halide_blur_glsl.generator
                           GENERATOR_ARGS target=host-opengl-debug)

    halide_generator(halide_ycc_glsl.generator
                         SRCS halide_ycc_glsl_generator.cpp)
    halide_library_from_generator(halide_ycc_glsl
                           GENERATOR halide_ycc_glsl.generator
                           GENERATOR_ARGS target=host-opengl-debug)

    # Final executable
    add_executable(opengl_test opengl_test.cpp)
    target_link_libraries(opengl_test PUBLIC halide_blur_glsl)
    target_link_libraries(opengl_test PUBLIC halide_ycc_glsl)

    if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        target_link_libraries(opengl_test PRIVATE pthread dl X11 GL)
    endif()

endif()

