include(CMakeParseArguments)

function(halide_use_image_io TARGET)
  foreach(PKG PNG JPEG)
    # It's OK to call find_package() for the same package multiple times.
    find_package(${PKG} QUIET)
    if(${PKG}_FOUND)
      target_compile_definitions(${TARGET} PRIVATE ${${PKG}_DEFINITIONS})
      target_include_directories(${TARGET} PRIVATE ${${PKG}_INCLUDE_DIRS})
      target_link_libraries(${TARGET} PRIVATE ${${PKG}_LIBRARIES})
    else()
      message(STATUS "${PKG} not found for ${TARGET}; compiling with -DHALIDE_NO_${PKG}")
      target_compile_definitions(${TARGET} PRIVATE -DHALIDE_NO_${PKG})
    endif()
  endforeach()
endfunction()

# If paths to tools, include, and libHalide aren't specified, infer them
# based on the path to the distrib folder. If the path to the distrib
# folder isn't specified, fail.
if("${HALIDE_TOOLS_DIR}" STREQUAL "" OR 
    "${HALIDE_INCLUDE_DIR}" STREQUAL "" OR 
    "${HALIDE_COMPILER_LIB}" STREQUAL "")
  if("${HALIDE_DISTRIB_DIR}" STREQUAL "")
    message(FATAL_ERROR "HALIDE_DISTRIB_DIR must point to the Halide distribution directory.")
  endif()
  set(HALIDE_INCLUDE_DIR "${HALIDE_DISTRIB_DIR}/include")
  set(HALIDE_TOOLS_DIR "${HALIDE_DISTRIB_DIR}/tools")
  add_library(_halide_compiler_lib INTERFACE)
  set_target_properties(_halide_compiler_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${HALIDE_INCLUDE_DIR})
  set_target_properties(_halide_compiler_lib PROPERTIES INTERFACE_LINK_LIBRARIES "${HALIDE_DISTRIB_DIR}/lib/libHalide${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(HALIDE_COMPILER_LIB _halide_compiler_lib)
endif()

if("${HALIDE_SYSTEM_LIBS}" STREQUAL "")
  # If HALIDE_SYSTEM_LIBS isn't defined, we are compiling against a Halide distribution
  # folder; this is normally captured in the halide_config.cmake file. If that file
  # exists in the same directory as this one, just include it here.
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/halide_config.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/halide_config.cmake")
  else()
    message(FATAL_ERROR "HALIDE_SYSTEM_LIBS is not set and we could not find halide_config.cmake")
  endif()
endif()

define_property(TARGET PROPERTY HALIDE_GENERATOR_NAME
                BRIEF_DOCS "Internal use by Halide build rules"
                FULL_DOCS "Internal use by Halide build rules")

function(_set_cxx_options TARGET)
  set_target_properties("${TARGET}" PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  if (MSVC)
    target_compile_definitions("${TARGET}" PUBLIC "-D_CRT_SECURE_NO_WARNINGS" "-D_SCL_SECURE_NO_WARNINGS")
    target_compile_options("${TARGET}" PRIVATE "/GR-")
  else()
    target_compile_options("${TARGET}" PRIVATE "-fno-rtti")
  endif()
endfunction()

function(_halide_generator_genfiles_dir NAME OUTVAR)
  set(GENFILES_DIR "${CMAKE_BINARY_DIR}/genfiles/${NAME}")
  file(MAKE_DIRECTORY "${GENFILES_DIR}")
  set(${OUTVAR} "${GENFILES_DIR}" PARENT_SCOPE)
endfunction()

# Adds features to a target string, canonicalizing the result.
# If multitarget, features are added to all.
function(_add_target_features HALIDE_TARGET HALIDE_FEATURES OUTVAR)
  string(REPLACE "," ";" MULTITARGETS "${HALIDE_TARGET}")
  foreach(T ${MULTITARGETS})
    string(REPLACE "-" ";" NEW_T "${T}")
    foreach(F ${HALIDE_FEATURES})
      list(APPEND NEW_T ${F})
    endforeach()
    string(REPLACE ";" "-" NEW_T "${NEW_T}")
    _canonicalize_target("${NEW_T}" NEW_T)    
    list(APPEND NEW_MULTITARGETS ${NEW_T})
  endforeach()
  string(REPLACE ";" "," NEW_MULTITARGETS "${NEW_MULTITARGETS}")
  set(${OUTVAR} "${NEW_MULTITARGETS}" PARENT_SCOPE)
endfunction(_add_target_features)

# Split the target into base and feature lists.
function(_split_target HALIDE_TARGET OUTVAR_BASE OUTVAR_FEATURES)
  if("${HALIDE_TARGET}" MATCHES ".*,.*")
    message(FATAL_ERROR "Multitarget may not be specified in _split_target(${HALIDE_TARGET})")
  endif()

  string(REPLACE "-" ";" FEATURES "${HALIDE_TARGET}")
  list(LENGTH FEATURES LEN)
  if("${LEN}" EQUAL 0)
    message(FATAL_ERROR "Empty target")
  endif()

  list(GET FEATURES 0 BASE)
  if ("${BASE}" STREQUAL "host")
    list(REMOVE_AT FEATURES 0)
  else()
    if("${LEN}" LESS 3)
      message(FATAL_ERROR "Illegal target (${HALIDE_TARGET})")
    endif()
    list(GET FEATURES 0 1 2 BASE)
    list(REMOVE_AT FEATURES 0 1 2)
  endif()
  set(${OUTVAR_BASE} "${BASE}" PARENT_SCOPE)
  set(${OUTVAR_FEATURES} "${FEATURES}" PARENT_SCOPE)
endfunction(_split_target)

# Join base and feature lists back into a target. Do not canonicalize.
function(_join_target BASE FEATURES OUTVAR)
  foreach(F ${FEATURES})
    list(APPEND BASE ${F})
  endforeach()
  string(REPLACE ";" "-" BASE "${BASE}")
  set(${OUTVAR} "${BASE}" PARENT_SCOPE)
endfunction(_join_target)

# Alphabetizes the features part of the target to make sure they always match no
# matter the concatenation order of the target string pieces. Remove duplicates.
function(_canonicalize_target HALIDE_TARGET OUTVAR)
  if("${HALIDE_TARGET}" MATCHES ".*,.*")
    message(FATAL_ERROR "Multitarget may not be specified in _canonicalize_target(${HALIDE_TARGET})")
  endif()
  _split_target("${HALIDE_TARGET}" BASE FEATURES)
  list(REMOVE_DUPLICATES FEATURES)
  list(SORT FEATURES)
  _join_target("${BASE}" "${FEATURES}" HALIDE_TARGET)
  set(${OUTVAR} "${HALIDE_TARGET}" PARENT_SCOPE)
endfunction(_canonicalize_target)

# Given a HALIDE_TARGET return the CMake target name for the runtime.
function(_halide_library_runtime_target_name HALIDE_TARGET OUTVAR)
  # MULTITARGETS = HALIDE_TARGET.split(",")
  string(REPLACE "," ";" MULTITARGETS "${HALIDE_TARGET}")
  # HALIDE_TARGET = MULTITARGETS.final_element()
  list(GET MULTITARGETS -1 HALIDE_TARGET)
  _canonicalize_target("${HALIDE_TARGET}" HALIDE_TARGET)
  _split_target("${HALIDE_TARGET}" BASE FEATURES)
  # Discard target features which do not affect the contents of the runtime.
  list(REMOVE_DUPLICATES FEATURES)
  list(REMOVE_ITEM FEATURES "user_context" "no_asserts" "no_bounds_query" "profile")
  list(SORT FEATURES)
  # Now build up the name
  set(RESULT "halide_library_runtime")
  foreach(B ${BASE})
    list(APPEND RESULT ${B})
  endforeach()
  foreach(F ${FEATURES})
    list(APPEND RESULT ${F})
  endforeach()
  string(REPLACE ";" "_" RESULT "${RESULT}")
  set(${OUTVAR} "${RESULT}" PARENT_SCOPE)
endfunction(_halide_library_runtime_target_name)

function(_halide_generator_add_exec_generator_target EXEC_TARGET)
  set(options )
  set(oneValueArgs GENERATOR_BINARY)
  set(multiValueArgs OUTPUTS GENERATOR_ARGS)
  cmake_parse_arguments(args "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  add_custom_target(${EXEC_TARGET} DEPENDS ${args_OUTPUTS})
  set_target_properties(${EXEC_TARGET} PROPERTIES FOLDER "generator")

  # As of CMake 3.x, add_custom_command() recognizes executable target names in its COMMAND.
  add_custom_command(
    OUTPUT ${args_OUTPUTS}
    DEPENDS ${args_GENERATOR_BINARY}
    COMMAND ${args_GENERATOR_BINARY} ${args_GENERATOR_ARGS}
  )
  foreach(OUT ${args_OUTPUTS})
    set_source_files_properties(${OUT} PROPERTIES GENERATED TRUE)
  endforeach()
endfunction()

function(halide_generator NAME)
# TODO DEPS other than stubs
  set(oneValueArgs GENERATOR_NAME)
  set(multiValueArgs SRCS DEPS INCLUDES)
  cmake_parse_arguments(args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ${NAME} MATCHES "^.*\\.generator$")
    message(FATAL_ERROR "halide_generator rules must have names that end in .generator (${NAME})")
  endif()

  # BASENAME = strip_suffix(${NAME}, ".generator")
  string(REGEX REPLACE "\\.generator*$" "" BASENAME ${NAME})

  if ("${args_GENERATOR_NAME}" STREQUAL "")
    set(args_GENERATOR_NAME "${BASENAME}")
  endif()

  list(LENGTH args_SRCS SRCSLEN)
  # Don't create empty object-library: that can cause quiet failures in MSVC builds.
  if("${SRCSLEN}" GREATER 0)
    # Use Object Libraries to so that Generator registration isn't dead-stripped away
    set(OBJLIB "${NAME}_library")
    add_library("${OBJLIB}" OBJECT ${args_SRCS})
    # Ensure that Halide.h is built prior to any Generator
    add_dependencies("${OBJLIB}" ${HALIDE_COMPILER_LIB})
    target_include_directories("${OBJLIB}" PRIVATE 
                               "${args_INCLUDES}"
                               "${HALIDE_INCLUDE_DIR}" 
                               "${HALIDE_TOOLS_DIR}")
    _set_cxx_options("${OBJLIB}")
    # TODO: this needs attention so that it can work with "ordinary" deps (e.g. static libs)
    # as well as generator deps (which are object-libraries which need special casing)
    set(ALLDEPS $<TARGET_OBJECTS:${OBJLIB}>)
    foreach(DEP ${args_DEPS})
      list(APPEND ALLDEPS $<TARGET_OBJECTS:${DEP}_library>)
      add_dependencies("${OBJLIB}" "${DEP}")
      target_include_directories("${OBJLIB}" PRIVATE $<TARGET_PROPERTY:${DEP},INTERFACE_INCLUDE_DIRECTORIES>)
    endforeach()
  endif()

  add_executable("${NAME}_binary" ${ALLDEPS} "${HALIDE_TOOLS_DIR}/GenGen.cpp")
  _set_cxx_options("${NAME}_binary")
  target_link_libraries("${NAME}_binary" PRIVATE ${HALIDE_COMPILER_LIB} ${HALIDE_SYSTEM_LIBS} ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
  target_include_directories("${NAME}_binary" PRIVATE "${HALIDE_TOOLS_DIR}")
  set_target_properties("${NAME}_binary" PROPERTIES FOLDER "generator")
  if (MSVC)
    target_link_libraries("${NAME}_binary" PRIVATE Kernel32)
  endif()

  _halide_generator_genfiles_dir(${BASENAME} GENFILES_DIR)
  set(STUB_HDR "${GENFILES_DIR}/${BASENAME}.stub.h")
  set(GENERATOR_EXEC_ARGS "-o" "${GENFILES_DIR}" "-e" "cpp_stub" "-n" "${BASENAME}")

  _halide_generator_add_exec_generator_target(
    "${NAME}_stub_gen"
    GENERATOR_BINARY "${NAME}_binary"
    GENERATOR_ARGS   "${GENERATOR_EXEC_ARGS}"
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

# Generate the runtime library for the given halide_target; return
# its cmake target name in outvar.
function(_halide_library_runtime GENERATOR_HALIDE_TARGET OUTVAR)
  if(NOT TARGET halide_library_runtime.generator)
    halide_generator(halide_library_runtime.generator SRCS "")
  endif()

  string(REPLACE "," ";" MULTITARGETS "${GENERATOR_HALIDE_TARGET}")
  list(GET MULTITARGETS -1 GENERATOR_HALIDE_TARGET)
  _halide_library_runtime_target_name("${GENERATOR_HALIDE_TARGET}" RUNTIME_NAME)
  if(NOT TARGET "${RUNTIME_NAME}")
    set(RUNTIME_LIB "${RUNTIME_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}")

    _halide_generator_genfiles_dir(${RUNTIME_NAME} GENFILES_DIR)
    set(GENERATOR_EXEC_ARGS "-o" "${GENFILES_DIR}")
    list(APPEND GENERATOR_EXEC_ARGS "-r" "${RUNTIME_NAME}")
    list(APPEND GENERATOR_EXEC_ARGS "target=${GENERATOR_HALIDE_TARGET}")

    _halide_generator_add_exec_generator_target(
      "${RUNTIME_NAME}_runtime_gen"
      GENERATOR_BINARY "halide_library_runtime.generator_binary"
      GENERATOR_ARGS   "${GENERATOR_EXEC_ARGS}"
      OUTPUTS          "${GENFILES_DIR}/${RUNTIME_LIB}"
    )

    add_library("${RUNTIME_NAME}" INTERFACE)
    add_dependencies("${RUNTIME_NAME}" "${RUNTIME_NAME}_runtime_gen")
    set_target_properties("${RUNTIME_NAME}" PROPERTIES INTERFACE_LINK_LIBRARIES "${GENFILES_DIR}/${RUNTIME_LIB}")
  endif()
  set(${OUTVAR} "${RUNTIME_NAME}" PARENT_SCOPE)  
endfunction(_halide_library_runtime)

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
  # Select the runtime to use *before* adding no_runtime
  _halide_library_runtime("${args_GENERATOR_HALIDE_TARGET}" RUNTIME_NAME)
  _add_target_features("${args_GENERATOR_HALIDE_TARGET}" "no_runtime" args_GENERATOR_HALIDE_TARGET)
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
    OUTPUTS          ${OUTPUTS}
  )

  # -------------------

  add_library("${BASENAME}" INTERFACE)
  add_dependencies("${BASENAME}" "${BASENAME}_lib_gen")
  if (${_h_index} GREATER -1)
    set_target_properties("${BASENAME}" PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${GENFILES_DIR})
  endif()
  if (${_lib_index} GREATER -1)
    add_dependencies("${BASENAME}" "${RUNTIME_NAME}")
    set_target_properties("${BASENAME}" PROPERTIES 
      INTERFACE_LINK_LIBRARIES "${GENFILES_DIR}/${FILTER_LIB};${RUNTIME_NAME};${CMAKE_DL_LIBS};${CMAKE_THREAD_LIBS_INIT}")
  endif()


  # ------ Code to build the RunGen target
  if(NOT TARGET _halide_library_from_generator_rungen)
    add_library(_halide_library_from_generator_rungen "${HALIDE_TOOLS_DIR}/RunGen.cpp")
    target_include_directories(_halide_library_from_generator_rungen PRIVATE "${HALIDE_INCLUDE_DIR}")
    halide_use_image_io(_halide_library_from_generator_rungen)
    _set_cxx_options(_halide_library_from_generator_rungen)
  endif()

  set(RUNGEN "${BASENAME}.rungen")
  add_executable("${RUNGEN}" "${HALIDE_TOOLS_DIR}/RunGenStubs.cpp")
  target_compile_definitions("${RUNGEN}" PRIVATE "-DHL_RUNGEN_FILTER_HEADER=\"${BASENAME}.h\"")
  target_link_libraries("${RUNGEN}" PRIVATE _halide_library_from_generator_rungen "${BASENAME}" ${args_FILTER_DEPS})

  # Not all Generators will build properly with RunGen (e.g., missing
  # external dependencies), so exclude them from the "ALL" targets
  set_target_properties("${RUNGEN}" PROPERTIES EXCLUDE_FROM_ALL TRUE)

  add_custom_target("${BASENAME}.run" 
                    COMMAND "${RUNGEN}" "$(RUNARGS)"
                    DEPENDS "${RUNGEN}")
  set_target_properties("${BASENAME}.run" PROPERTIES EXCLUDE_FROM_ALL TRUE)

endfunction(halide_library_from_generator)

function(halide_library NAME)
  set(oneValueArgs GENERATOR_NAME FUNCTION_NAME GENERATOR_HALIDE_TARGET)
  set(multiValueArgs SRCS GENERATOR_DEPS FILTER_DEPS INCLUDES GENERATOR_ARGS GENERATOR_OUTPUTS)
  cmake_parse_arguments(args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  halide_generator("${NAME}.generator"
                   SRCS ${args_SRCS}
                   DEPS ${args_GENERATOR_DEPS}
                   INCLUDES ${args_INCLUDES}
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

  add_library(${AOT_LIBRARY_TARGET}.cpplib STATIC "${GENFILES_DIR}/${AOT_LIBRARY_TARGET}.cpp")
  target_link_libraries("${TARGET}" PRIVATE ${AOT_LIBRARY_TARGET}.cpplib)
  target_include_directories("${TARGET}" PRIVATE "${GENFILES_DIR}")
endfunction(halide_add_aot_cpp_dependency)

