# Generator(s)
halide_add_generator(pipeline.generator
                     SRCS pipeline_generator.cpp)
halide_add_aot_library(pipeline_c
                       GENERATOR_TARGET pipeline.generator
                       GENERATED_FUNCTION pipeline_c
                       GENERATOR_OUTPUTS cpp h
                       GENERATOR_ARGS target=host)

halide_add_aot_library(pipeline_native
                       GENERATOR_TARGET pipeline.generator
                       GENERATED_FUNCTION pipeline_native
                       GENERATOR_OUTPUTS static_library h
                       GENERATOR_ARGS target=host)

halide_add_generator(pipeline_cpp.generator
                     SRCS pipeline_cpp_generator.cpp)
halide_add_aot_library(pipeline_cpp_cpp
                       GENERATOR_TARGET pipeline_cpp.generator
                       GENERATED_FUNCTION pipeline_cpp_cpp
                       GENERATOR_OUTPUTS cpp h
                       GENERATOR_ARGS target=host-c_plus_plus_name_mangling)
halide_add_aot_library(pipeline_cpp_native
                       GENERATOR_TARGET pipeline_cpp.generator
                       GENERATED_FUNCTION pipeline_cpp_native
                       GENERATOR_OUTPUTS static_library h
                       GENERATOR_ARGS target=host-c_plus_plus_name_mangling)

# Final executable(s)
add_executable(run_c_backend_and_native run.cpp)
halide_add_aot_cpp_dependency(run_c_backend_and_native pipeline_c)
halide_add_aot_library_dependency(run_c_backend_and_native pipeline_native)

add_executable(run_c_backend_and_native_cpp run_cpp.cpp)
halide_add_aot_cpp_dependency(run_c_backend_and_native_cpp pipeline_cpp_cpp)
halide_add_aot_library_dependency(run_c_backend_and_native_cpp pipeline_cpp_native)






