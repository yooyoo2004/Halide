include(CMakeParseArguments)

function(_halide_generator_genfiles_dir NAME OUTVAR)
  set(GENFILES_DIR "${CMAKE_BINARY_DIR}/genfiles/${NAME}")
  file(MAKE_DIRECTORY "${GENFILES_DIR}")
  set(${OUTVAR} "${GENFILES_DIR}" PARENT_SCOPE)
endfunction()

function(_halide_generator_add_exec_generator_target EXEC_TARGET)
  set(options )
  set(oneValueArgs GENERATOR GENFILES_DIR)
  set(multiValueArgs OUTPUTS GENERATOR_ARGS)
  cmake_parse_arguments(args "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(MSVC)
    # In MSVC, the generator executable will be placed in a configuration specific
    # directory specified by ${CMAKE_CFG_INTDIR}.
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/${CMAKE_CFG_INTDIR}/${args_GENERATOR}${CMAKE_EXECUTABLE_SUFFIX}")
  elseif(XCODE)
    # In Xcode, the generator executable will be placed in a configuration specific
    # directory, so the Xcode variable $(CONFIGURATION) is passed in the custom build script.
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/$(CONFIGURATION)/${args_GENERATOR}${CMAKE_EXECUTABLE_SUFFIX}")
  else()
    set(EXEC_PATH "${CMAKE_BINARY_DIR}/bin/${args_GENERATOR}${CMAKE_EXECUTABLE_SUFFIX}")
  endif()

  # This "target" will always be built.
  add_custom_target(${EXEC_TARGET} DEPENDS ${args_OUTPUTS})
  set_target_properties(${EXEC_TARGET} PROPERTIES FOLDER "generator")
  add_custom_command(
    OUTPUT ${args_OUTPUTS}
    DEPENDS ${args_GENERATOR}
    COMMAND ${EXEC_PATH} ${args_GENERATOR_ARGS}
    WORKING_DIRECTORY ${args_GENFILES_DIR}
    COMMENT "Executing Generator ${args_GENERATOR} with args ${args_GENERATOR_ARGS}..."
  )
  foreach(OUT ${args_OUTPUTS})
    set_source_files_properties(${OUT} PROPERTIES GENERATED TRUE)
  endforeach()
endfunction()

function(halide_generator NAME)
# TODO GENERATOR_NAME
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

  add_executable("${NAME}_binary" "${CMAKE_SOURCE_DIR}/tools/GenGen.cpp" ${ALLDEPS})
  target_link_libraries("${NAME}_binary" PRIVATE ${args_LIBHALIDE} z ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
  target_include_directories("${NAME}_binary" PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/tools")
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
    GENERATOR "${NAME}_binary"
    GENERATOR_ARGS   "${GENERATOR_EXEC_ARGS}"
    GENFILES_DIR     "${GENFILES_DIR}"
    OUTPUTS          "${STUB_HDR}"
  )

  # Make a header-only library that exports the include path
  add_library("${NAME}" INTERFACE)
  add_dependencies("${NAME}" "${NAME}_stub_gen")
  set_target_properties("${NAME}" PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${GENFILES_DIR})
endfunction(halide_generator)

function(halide_library_from_generator BASENAME)
  set(options )
  set(oneValueArgs GENERATOR GENERATOR_NAME GENERATED_FUNCTION)
  set(multiValueArgs GENERATOR_ARGS GENERATOR_OUTPUTS FILTER_DEPS)
  cmake_parse_arguments(args "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if (args_GENERATED_FUNCTION STREQUAL "")
    set(args_GENERATED_FUNCTION ${args_GENERATOR_NAME})
  endif()

  # Create a directory to contain generator specific intermediate files
  _halide_generator_genfiles_dir(${BASENAME} GENFILES_DIR)

  # Determine the name of the output files
  set(FILTER_LIB "${BASENAME}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(FILTER_HDR "${BASENAME}.h")
  set(FILTER_CPP "${BASENAME}.cpp")

  set(GENERATOR_EXEC_ARGS "-o" "${GENFILES_DIR}")
  if (NOT ${args_GENERATED_FUNCTION} STREQUAL "")
    list(APPEND GENERATOR_EXEC_ARGS "-f" "${args_GENERATED_FUNCTION}" )
  endif()
  if (NOT ${args_GENERATOR_NAME} STREQUAL "")
    list(APPEND GENERATOR_EXEC_ARGS "-g" "${args_GENERATOR_NAME}")
  endif()
  if (NOT "${args_GENERATOR_OUTPUTS}" STREQUAL "")
    string(REPLACE ";" "," _tmp "${args_GENERATOR_OUTPUTS}")
    list(APPEND GENERATOR_EXEC_ARGS "-e" ${_tmp})
  endif()
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
    GENERATOR "${args_GENERATOR}_binary"
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
  add_executable("${RUNGEN}" "${CMAKE_SOURCE_DIR}/tools/RunGenStubs.cpp")
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

function(halide_add_aot_cpp_dependency TARGET AOT_LIBRARY_TARGET)
  _halide_generator_genfiles_dir(${AOT_LIBRARY_TARGET} GENFILES_DIR)
  # add_dependencies("${TARGET}" "${AOT_LIBRARY_TARGET}.exec_generator")

  add_library(${AOT_LIBRARY_TARGET}.cpplib STATIC "${GENFILES_DIR}/${AOT_LIBRARY_TARGET}.cpp")
  target_link_libraries("${TARGET}" PRIVATE ${AOT_LIBRARY_TARGET}.cpplib)
  target_include_directories("${TARGET}" PRIVATE "${GENFILES_DIR}")
endfunction(halide_add_aot_cpp_dependency)

