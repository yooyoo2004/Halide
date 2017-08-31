add_executable(build_halide_h build_halide_h.cpp)
add_executable(binary2cpp binary2cpp.cpp)
if (MSVC)
  # disable irrelevant warnings
  target_compile_options(build_halide_h PUBLIC /wd4996)
  target_compile_options(binary2cpp PUBLIC /wd4996)
endif()

