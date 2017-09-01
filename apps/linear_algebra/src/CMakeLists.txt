set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

halide_generator(blas_l1.generator SRCS blas_l1_generators.cpp)
halide_generator(blas_l2.generator SRCS blas_l2_generators.cpp)
halide_generator(blas_l3.generator SRCS blas_l3_generators.cpp)

add_library(halide_blas
  halide_blas.cpp
)

set(FEATURES no_asserts no_bounds_query)

halide_library(halide_scopy_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME saxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=false add_to_y=false
)
target_link_libraries(halide_blas PUBLIC halide_scopy_impl)

halide_library(halide_dcopy_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME daxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=false add_to_y=false
)
target_link_libraries(halide_blas PUBLIC halide_dcopy_impl)

halide_library(halide_sscal_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME saxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=true add_to_y=false
)
target_link_libraries(halide_blas PUBLIC halide_sscal_impl)

halide_library(halide_dscal_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME daxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=true add_to_y=false
)
target_link_libraries(halide_blas PUBLIC halide_dscal_impl)

halide_library(halide_saxpy_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME saxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=true add_to_y=true
)
target_link_libraries(halide_blas PUBLIC halide_saxpy_impl)

halide_library(halide_daxpy_impl
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME daxpy 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true scale_x=true add_to_y=true
)
target_link_libraries(halide_blas PUBLIC halide_daxpy_impl)

halide_library(halide_sdot
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME sdot 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_sdot)

halide_library(halide_ddot
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME ddot 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_ddot)

halide_library(halide_sasum
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME sasum 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_sasum)

halide_library(halide_dasum
	SRCS blas_l1_generators.cpp
	GENERATOR_NAME dasum 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_dasum)

halide_library(halide_sgemv_notrans
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME sgemv 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true transpose=false
)
target_link_libraries(halide_blas PUBLIC halide_sgemv_notrans)

halide_library(halide_dgemv_notrans
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME dgemv 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true transpose=false
)
target_link_libraries(halide_blas PUBLIC halide_dgemv_notrans)

halide_library(halide_sgemv_trans
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME sgemv 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true transpose=true
)
target_link_libraries(halide_blas PUBLIC halide_sgemv_trans)

halide_library(halide_dgemv_trans
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME dgemv 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true transpose=true
)
target_link_libraries(halide_blas PUBLIC halide_dgemv_trans)

halide_library(halide_sger_impl
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME sger 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_sger_impl)

halide_library(halide_dger_impl
	SRCS blas_l2_generators.cpp
	GENERATOR_NAME dger 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS parallel=false vectorize=true
)
target_link_libraries(halide_blas PUBLIC halide_dger_impl)

halide_library(halide_sgemm_notrans
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME sgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=false transpose_B=false
)
target_link_libraries(halide_blas PUBLIC halide_sgemm_notrans)

halide_library(halide_dgemm_notrans
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME dgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=false transpose_B=false
)
target_link_libraries(halide_blas PUBLIC halide_dgemm_notrans)

halide_library(halide_sgemm_transA
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME sgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=true transpose_B=false
)
target_link_libraries(halide_blas PUBLIC halide_sgemm_transA)

halide_library(halide_dgemm_transA
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME dgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=true transpose_B=false
)
target_link_libraries(halide_blas PUBLIC halide_dgemm_transA)

halide_library(halide_sgemm_transB
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME sgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=false transpose_B=true
)
target_link_libraries(halide_blas PUBLIC halide_sgemm_transB)

halide_library(halide_dgemm_transB
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME dgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=false transpose_B=true
)
target_link_libraries(halide_blas PUBLIC halide_dgemm_transB)

halide_library(halide_sgemm_transAB
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME sgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=true transpose_B=true
)
target_link_libraries(halide_blas PUBLIC halide_sgemm_transAB)

halide_library(halide_dgemm_transAB
	SRCS blas_l3_generators.cpp
	GENERATOR_NAME dgemm 
    HALIDE_TARGET_FEATURES ${FEATURES}
    GENERATOR_ARGS transpose_A=true transpose_B=true
)
target_link_libraries(halide_blas PUBLIC halide_dgemm_transAB)
