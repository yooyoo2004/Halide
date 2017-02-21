#include "Halide.h"

using Halide::Buffer;
using Halide::Invoker;

const int kSize = 32;

void verify(const Buffer<int32_t> &img, float compiletime_factor, float runtime_factor, int channels) {
    for (int i = 0; i < kSize; i++) {
        for (int j = 0; j < kSize; j++) {
            for (int c = 0; c < channels; c++) {
                if (img(i, j, c) !=
                    (int32_t)(compiletime_factor * runtime_factor * c * (i > j ? i : j))) {
                    printf("img[%d, %d, %d] = %d\n", i, j, c, img(i, j, c));
                    exit(-1);
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    Halide::JITGeneratorContext context(Halide::get_target_from_environment());

    {
        auto example = Invoker(context, "example")
            // Optionally set GeneratorParams before calling generate().
            .set_generator_param("compiletime_factor", 2.5f)
            // You can also use string values.
            .set_generator_param("enummy", "foo")
            // Pass the Inputs, in order, to Invoker's generate() method.
            // This sets the Input<> values correctly. If you pass the wrong number of Inputs,
            // or the wrong type for a given Input (e.g. int where a float is expected),
            // Halide-compile-fail.
            .generate(1.f)
            // We can go ahead and call schedule now when jitting.
            .schedule();

        Halide::Buffer<int32_t> img = example.realize(kSize, kSize, 3);
        verify(img, 2.5f, 1, 3);
    }

    {
        // Use defaults for all GeneratorParams.
        auto example = Invoker(context, "example")
            .generate(1.f)
            // We'll set "vectorize=false" in the ScheduleParams, just to
            // show that we can:
            .set_schedule_param("vectorize", false)
            .schedule();

        Halide::Buffer<int32_t> img(kSize, kSize, 3);
        example.realize(img);
        verify(img, 1, 1, 3);
    }

    printf("Success!\n");
    return 0;
}
