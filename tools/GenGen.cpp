#include "Halide.h"

#ifndef HL_GENGEN_GENERATOR_NAME
    // #error "HL_GENGEN_GENERATOR_NAME must be defined"
#endif

// #define HL_COMBINE_NAMES_(A, B) A##B
// #define HL_COMBINE_NAMES(A, B) HL_COMBINE_NAMES_(A, B)

// extern "C" void 
// HL_COMBINE_NAMES(hl_halide_register_generator_factory_, HL_GENGEN_GENERATOR_NAME)
//     (const Halide::GeneratorContext& context, 
//      const std::map<std::string, std::string> &params, 
//      std::unique_ptr<Halide::Internal::GeneratorBase> *generator_out);

int main(int argc, char **argv) {
  return Halide::Internal::generate_filter_main(
        nullptr, // HL_COMBINE_NAMES(hl_halide_register_generator_factory_, HL_GENGEN_GENERATOR_NAME),
        argc, argv, std::cerr);
}



