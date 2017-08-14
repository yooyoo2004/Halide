include(CMakeParseArguments)

if("${HALIDE_SRC_DIR}" STREQUAL "")
  set(HALIDE_SRC_DIR "${CMAKE_SOURCE_DIR}")
endif()

define_property(TARGET PROPERTY HALIDE_GENERATOR_NAME
                BRIEF_DOCS "Internal use by Halide build rules"
                FULL_DOCS "Internal use by Halide build rules")

function(_halide_generator_genfiles_dir NAME OUTVAR)
  set(GENFILES_DIR "${CMAKE_BINARY_DIR}/genfiles/${NAME}")
  file(MAKE_DIRECTORY "${GENFILES_DIR}")
  set(${OUTVAR} "${GENFILES_DIR}" PARENT_SCOPE)
endfunction()

function(_halide_generator_add_exec_generator_target EXEC_TARGET)
  set(options )
  set(oneValueArgs GENERATOR_BINARY GENFILES_DIR)
  set(multiValueArgs OUTPUTS GENERATOR_ARGS)
  cmake_parse_arguments(args "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(MSVC)
    # In MSVC, the generator executable will be placed in a configuration specific
    # directory specified by ${CMAKE_CFG_INTDIR}.
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/${CMAKE_CFG_INTDIR}/${args_GENERATOR_BINARY}${CMAKE_EXECUTABLE_SUFFIX}")
  elseif(XCODE)
    # In Xcode, the generator executable will be placed in a configuration specific
    # directory, so the Xcode variable $(CONFIGURATION) is passed in the custom build script.
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/$(CONFIGURATION)/${args_GENERATOR_BINARY}${CMAKE_EXECUTABLE_SUFFIX}")
  else()
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/${args_GENERATOR_BINARY}${CMAKE_EXECUTABLE_SUFFIX}")
  endif()

  # This "target" will always be built.
  add_custom_target(${EXEC_TARGET} DEPENDS ${args_OUTPUTS})
  set_target_properties(${EXEC_TARGET} PROPERTIES FOLDER "generator")
  add_custom_command(
    OUTPUT ${args_OUTPUTS}
    DEPENDS ${args_GENERATOR_BINARY}
    COMMAND ${EXEC_PATH} ${args_GENERATOR_ARGS}
    WORKING_DIRECTORY ${args_GENFILES_DIR}
    # COMMENT "Executing Generator ${args_GENERATOR_BINARY} with args ${args_GENERATOR_ARGS}..."
  )
  foreach(OUT ${args_OUTPUTS})
    set_source_files_properties(${OUT} PROPERTIES GENERATED TRUE)
  endforeach()
endfunction()

function(halide_generator NAME)
# TODO DEPS other than stubs
  set(oneValueArgs LIBHALIDE GENERATOR_NAME)
  set(multiValueArgs SRCS DEPS INCLUDES)
  cmake_parse_arguments(args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if ("${args_LIBHALIDE}" STREQUAL "")
    set(args_LIBHALIDE Halide)
  endif()

  if(NOT ${NAME} MATCHES "^.*\\.generator$")
    message(FATAL_ERROR "halide_generator rules must have names that end in .generator (${NAME})")
  endif()

  # BASENAME = strip_suffix(${NAME}, ".generator")
  string(REGEX REPLACE "\\.generator*$" "" BASENAME ${NAME})

  if ("${args_GENERATOR_NAME}" STREQUAL "")
    set(args_GENERATOR_NAME "${BASENAME}")
  endif()

  # Use Object Libraries to so that Generator registration isn't dead-stripped away
  set(OBJLIB "${NAME}_library")
  add_library("${OBJLIB}" OBJECT ${args_SRCS})
  add_dependencies("${OBJLIB}" ${args_LIBHALIDE})  # ensure Halide.h is built
  target_include_directories("${OBJLIB}" PRIVATE "${CMAKE_BINARY_DIR}/include")
  target_include_directories("${OBJLIB}" PRIVATE "${args_INCLUDES}")
  set_target_properties("${OBJLIB}" PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  if (MSVC)
    target_compile_options("${OBJLIB}" PRIVATE "/GR-")
  else()
    target_compile_options("${OBJLIB}" PRIVATE "-fno-rtti")
  endif()

  # TODO: this needs attention so that it can work with "ordinary" deps (e.g. static libs)
  # as well as generator deps (which are object-libraries which need special casing)
  set(ALLDEPS $<TARGET_OBJECTS:${OBJLIB}>)
  foreach(DEP ${args_DEPS})
    list(APPEND ALLDEPS $<TARGET_OBJECTS:${DEP}_library>)
    add_dependencies("${OBJLIB}" "${DEP}")
    target_include_directories("${OBJLIB}" PRIVATE $<TARGET_PROPERTY:${DEP},INTERFACE_INCLUDE_DIRECTORIES>)
  endforeach()

  add_executable("${NAME}_binary" "${HALIDE_SRC_DIR}/tools/GenGen.cpp" ${ALLDEPS})
  target_link_libraries("${NAME}_binary" PRIVATE ${args_LIBHALIDE} z ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
  target_include_directories("${NAME}_binary" PRIVATE "${HALIDE_SRC_DIR}/include" "${HALIDE_SRC_DIR}/tools")
  set_target_properties("${NAME}_binary" PROPERTIES FOLDER "generator")
  if (MSVC)
    set_target_properties(${NAME} PROPERTIES LINK_FLAGS "/ignore:4006 /ignore:4088")
    target_compile_definitions("${NAME}_binary" PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries("${NAME}_binary" PRIVATE Kernel32)
  endif()

  _halide_generator_genfiles_dir(${BASENAME} GENFILES_DIR)
  set(STUB_HDR "${GENFILES_DIR}/${BASENAME}.stub.h")
  set(GENERATOR_EXEC_ARGS "-o" "${GENFILES_DIR}" "-e" "cpp_stub" "-n" "${BASENAME}")

  _halide_generator_add_exec_generator_target(
    "${NAME}_stub_gen"
    GENERATOR_BINARY "${NAME}_binary"
    GENERATOR_ARGS   "${GENERATOR_EXEC_ARGS}"
    GENFILES_DIR     "${GENFILES_DIR}"
    OUTPUTS          "${STUB_HDR}"
  )
  set_property(TARGET "${NAME}_stub_gen" PROPERTY HALIDE_GENERATOR_NAME "${args_GENERATOR_NAME}")

  # Make a header-only library that exports the include path
  add_library("${NAME}" INTERFACE)
  add_dependencies("${NAME}" "${NAME}_stub_gen")
  set_target_properties("${NAME}" PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${GENFILES_DIR})
  # TODO -- not allowed, but this is what we need, yuck
  # set_target_properties("${NAME}" PROPERTIES INTERFACE_LINK_LIBRARIES ${OBJLIB})
endfunction(halide_generator)

# TODO do we want to use halide_target_features instead of GENERATOR_HALIDE_TARGET?
function(halide_library_from_generator BASENAME)
  set(options )
  set(oneValueArgs GENERATOR FUNCTION_NAME GENERATOR_HALIDE_TARGET)
  set(multiValueArgs GENERATOR_ARGS GENERATOR_OUTPUTS FILTER_DEPS)
  cmake_parse_arguments(args "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if ("${args_GENERATOR}" STREQUAL "")
    set(args_GENERATOR "${BASENAME}.generator")
  endif()

  get_property(GENERATOR_NAME TARGET "${args_GENERATOR}_stub_gen" PROPERTY HALIDE_GENERATOR_NAME)

  if ("${args_FUNCTION_NAME}" STREQUAL "")
    set(args_FUNCTION_NAME "${BASENAME}")
  endif()

  # Create a directory to contain generator specific intermediate files
  _halide_generator_genfiles_dir(${BASENAME} GENFILES_DIR)

  # Determine the name of the output files
  set(FILTER_LIB "${BASENAME}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(FILTER_HDR "${BASENAME}.h")
  set(FILTER_CPP "${BASENAME}.cpp")

  set(GENERATOR_EXEC_ARGS "-o" "${GENFILES_DIR}")
  if (NOT ${args_FUNCTION_NAME} STREQUAL "")
    list(APPEND GENERATOR_EXEC_ARGS "-f" "${args_FUNCTION_NAME}" )
  endif()
  if (NOT ${GENERATOR_NAME} STREQUAL "")
    list(APPEND GENERATOR_EXEC_ARGS "-g" "${GENERATOR_NAME}")
  endif()
  if (NOT "${args_GENERATOR_OUTPUTS}" STREQUAL "")
    string(REPLACE ";" "," _tmp "${args_GENERATOR_OUTPUTS}")
    list(APPEND GENERATOR_EXEC_ARGS "-e" ${_tmp})
  endif()
  if ("${args_GENERATOR_HALIDE_TARGET}" STREQUAL "")
    set(args_GENERATOR_HALIDE_TARGET "host")
  endif()
  list(APPEND GENERATOR_EXEC_ARGS "target=${args_GENERATOR_HALIDE_TARGET}")
  # GENERATOR_ARGS always come last
  list(APPEND GENERATOR_EXEC_ARGS ${args_GENERATOR_ARGS})

  if ("${args_GENERATOR_OUTPUTS}" STREQUAL "")
    set(args_GENERATOR_OUTPUTS static_library h)
  endif()

  set(OUTPUTS )
  
  # This is the CMake idiom for "if foo in list"
  list(FIND args_GENERATOR_OUTPUTS "static_library" _lib_index)
  list(FIND args_GENERATOR_OUTPUTS "h" _h_index)
  list(FIND args_GENERATOR_OUTPUTS "cpp" _cpp_index)

  if (${_lib_index} GREATER -1)
    list(APPEND OUTPUTS "${GENFILES_DIR}/${FILTER_LIB}")
  endif()
  if (${_h_index} GREATER -1)
    list(APPEND OUTPUTS "${GENFILES_DIR}/${FILTER_HDR}")
  endif()
  if (${_cpp_index} GREATER -1)
    list(APPEND OUTPUTS "${GENFILES_DIR}/${FILTER_CPP}")
  endif()

  _halide_generator_add_exec_generator_target(
    "${BASENAME}_lib_gen"
    GENERATOR_BINARY "${args_GENERATOR}_binary"
    GENERATOR_ARGS   "${GENERATOR_EXEC_ARGS}"
    GENFILES_DIR     ${GENFILES_DIR}
    OUTPUTS          ${OUTPUTS}
  )

  # -------------------

  add_library("${BASENAME}" INTERFACE)
  add_dependencies("${BASENAME}" "${BASENAME}_lib_gen")
  if (${_h_index} GREATER -1)
    set_target_properties("${BASENAME}" PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${GENFILES_DIR})
  endif()
  if (${_lib_index} GREATER -1)
    set_target_properties("${BASENAME}" PROPERTIES INTERFACE_LINK_LIBRARIES "${GENFILES_DIR}/${FILTER_LIB}")
  endif()


  # ------ Code to build the RunGen target

  set(RUNGEN "${BASENAME}.rungen")
  add_executable("${RUNGEN}" "${HALIDE_SRC_DIR}/tools/RunGenStubs.cpp")
  target_compile_definitions("${RUNGEN}" PRIVATE "-DHL_RUNGEN_FILTER_HEADER=\"${BASENAME}.h\"")
  target_link_libraries("${RUNGEN}" PRIVATE HalideToolsRunGen "${BASENAME}" ${args_FILTER_DEPS})

  # Not all Generators will build properly with RunGen (e.g., missing
  # external dependencies), so exclude them from the "ALL" targets
  set_target_properties("${RUNGEN}" PROPERTIES EXCLUDE_FROM_ALL TRUE)

  add_custom_target("${BASENAME}.run" 
                    COMMAND "${RUNGEN}" "$(RUNARGS)"
                    DEPENDS "${RUNGEN}")
  set_target_properties("${BASENAME}.run" PROPERTIES EXCLUDE_FROM_ALL TRUE)

endfunction(halide_library_from_generator)

function(halide_library NAME)
  set(oneValueArgs LIBHALIDE GENERATOR_NAME FUNCTION_NAME GENERATOR_HALIDE_TARGET)
  set(multiValueArgs SRCS GENERATOR_DEPS FILTER_DEPS INCLUDES GENERATOR_ARGS GENERATOR_OUTPUTS)
  cmake_parse_arguments(args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  halide_generator("${NAME}.generator"
                   SRCS ${args_SRCS}
                   DEPS ${args_GENERATOR_DEPS}
                   INCLUDES ${args_INCLUDES}
                   LIBHALIDE ${args_LIBHALIDE}
                   GENERATOR_NAME ${args_GENERATOR_NAME})

  halide_library_from_generator("${NAME}"
                   DEPS ${args_FILTER_DEPS}
                   GENERATOR "${NAME}.generator"
                   FUNCTION_NAME ${args_FUNCTION_NAME}
                   GENERATOR_HALIDE_TARGET ${args_GENERATOR_HALIDE_TARGET}
                   GENERATOR_ARGS ${args_GENERATOR_ARGS}
                   GENERATOR_OUTPUTS ${args_GENERATOR_OUTPUTS})
endfunction(halide_library)

function(halide_add_aot_cpp_dependency TARGET AOT_LIBRARY_TARGET)
  _halide_generator_genfiles_dir(${AOT_LIBRARY_TARGET} GENFILES_DIR)
  # add_dependencies("${TARGET}" "${AOT_LIBRARY_TARGET}.exec_generator")

  add_library(${AOT_LIBRARY_TARGET}.cpplib STATIC "${GENFILES_DIR}/${AOT_LIBRARY_TARGET}.cpp")
  target_link_libraries("${TARGET}" PRIVATE ${AOT_LIBRARY_TARGET}.cpplib)
  target_include_directories("${TARGET}" PRIVATE "${GENFILES_DIR}")
endfunction(halide_add_aot_cpp_dependency)

