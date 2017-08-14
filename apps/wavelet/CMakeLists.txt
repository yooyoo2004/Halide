#
# NOTE: Unlike all other CMakeLists.txt in the apps/ folder, this
# is deliberately intended to be standalone (not included from the toplevel)
# in order to show the minimum scaffolding necessary to use ahead-of-time
# Generators in a simple app.
#
# To use:
# mkdir cmake_build && cd cmake_build && cmake .. && make -j8 && ./bin/wavelet ../../images/gray.png .

project(wavelet)
cmake_minimum_required(VERSION 3.1.3)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(use_image_io TARGET)
  target_compile_definitions(${TARGET} PRIVATE ${PNG_DEFINITIONS} ${JPEG_DEFINITIONS})
  target_include_directories(${TARGET} PRIVATE ${PNG_INCLUDE_DIRS} ${JPEG_INCLUDE_DIRS})
  target_link_libraries(${TARGET} PRIVATE ${PNG_LIBRARIES} ${JPEG_LIBRARIES})
endfunction()

include(../../HalideGenerator.cmake)

# HalideGenerator.cmake makes this assumptions
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_SOURCE_DIR "${HALIDE_SRC_DIR}")

SET(HALIDE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" CACHE PATH "Root of Halide source Tree")
include_directories ("${HALIDE_SRC_DIR}/apps/support")
include_directories("${HALIDE_SRC_DIR}/include")
include_directories("${HALIDE_SRC_DIR}/tools")

add_executable(wavelet "${CMAKE_CURRENT_SOURCE_DIR}/wavelet.cpp")
if (MSVC)
  target_compile_options(wavelet PRIVATE "/GR-")
else()
  target_compile_options(wavelet PRIVATE "-fno-rtti")
endif()

find_package(PNG)
find_package(JPEG)
use_image_io(wavelet)

add_library(HalideToolsRunGen "${HALIDE_SRC_DIR}/tools/RunGen.cpp")
use_image_io(HalideToolsRunGen)
if (MSVC)
  target_compile_options("${GEN_EXECUTABLE}" PRIVATE "/GR-")
else()
  target_compile_options(HalideToolsRunGen PUBLIC "-fno-rtti")
endif()

# GENS := *_generator.cpp
file(GLOB GENS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*_generator.cpp")

foreach(GEN_SRC ${GENS})
    # GEN_NAME := *.generator
    string(REPLACE "_generator.cpp" "" GEN_NAME "${GEN_SRC}")

    halide_library("${GEN_NAME}"
                    SRCS           ${GEN_SRC} 
                    LIBHALIDE      ${HALIDE_SRC_DIR}/lib/libHalide.a)
    target_link_libraries(wavelet PUBLIC "${GEN_NAME}")

endforeach()

