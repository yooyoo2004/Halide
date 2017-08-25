# Generator(s)
halide_generator(pipeline.generator
                     SRCS pipeline_generator.cpp)
halide_library_from_generator(pipeline_c
                       GENERATOR pipeline.generator
                       GENERATOR_OUTPUTS cpp h
                       GENERATOR_ARGS target=host)

halide_library_from_generator(pipeline_native
                       GENERATOR pipeline.generator
                       GENERATOR_OUTPUTS static_library h
                       GENERATOR_ARGS target=host)

halide_generator(pipeline_cpp.generator
                     SRCS pipeline_cpp_generator.cpp)
halide_library_from_generator(pipeline_cpp_cpp
                       GENERATOR pipeline_cpp.generator
                       GENERATOR_OUTPUTS cpp h
                       GENERATOR_ARGS target=host-c_plus_plus_name_mangling)
halide_library_from_generator(pipeline_cpp_native
                       GENERATOR pipeline_cpp.generator
                       GENERATOR_OUTPUTS static_library h
                       GENERATOR_ARGS target=host-c_plus_plus_name_mangling)

# Final executable(s)
add_executable(run_c_backend_and_native run.cpp)
halide_add_aot_cpp_dependency(run_c_backend_and_native pipeline_c)
target_link_libraries(run_c_backend_and_native PUBLIC pipeline_native)

add_executable(run_c_backend_and_native_cpp run_cpp.cpp)
halide_add_aot_cpp_dependency(run_c_backend_and_native_cpp pipeline_cpp_cpp)
target_link_libraries(run_c_backend_and_native_cpp PUBLIC pipeline_cpp_native)






