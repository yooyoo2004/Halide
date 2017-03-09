#include "Halide.h"

// Include the machine-generated .stub.h header file.
#include "example.stub.h"

using namespace Halide;

const int kSize = 32;

void verify(const Buffer<int32_t> &img, float compiletime_factor, float runtime_factor, int channels) {
    img.for_each_element([=](int x, int y, int c) {
        int expected = (int32_t)(compiletime_factor * runtime_factor * c * (x > y ? x : y));
        int actual = img(x, y, c);
        assert(expected == actual);
    });
}

int main(int argc, char **argv) {
    JITGeneratorContext context(get_target_from_environment());

    {
        // If we have access to the generated ".stub.h" header for a given
        // Generator, we get additional type checking at C++ compile time;
        // for instance, we can create a stub by just using an ordinary C++ ctor
        // andpassing it a GeneratorContext. Note that each of these functions
        // returns a self-reference, so chaining calls together in this way
        // is convenient and terse.
        auto gen = example(context)
            // If we want to selectively set GeneratorParams, we can use strongly-typed
            // setter functions to set them anytime before we call generate(); those
            // we don't set will stay at the default values. (We can set each multiple
            // times if we want to, with final call winning.)
            .set_compiletime_factor(2.5f)
            .set_enummy(Enum_enummy::foo)
            // When we call generate(), we must pass a suitable value for each Input
            // in the Generator.
            .generate(1.f)
            // Similarly, we can selectively set ScheduleParams using strongly-typed
            // setter functions anytime before we call schedule().
            .set_vectorize(false)
            .schedule();

        Buffer<int32_t> img = gen.realize(kSize, kSize, 3);
        verify(img, 2.5f, 1, 3);
    }

    {
        // If we don't have access to a .stub.h (or just don't want to use it for
        // whatever reason), we can still use a generic GeneratorStub by referencing
        // the registry name of the Generator we want to use. We won't get as
        // much checking at C++ compile time, but verification will still be done
        // at Halide compilation time, so you still won't be able to use
        // the Generator in an unsafe or incorrect way. This uses the generic
        // version of GeneratorStub in a way identical to the example above.
        auto gen = GeneratorStub(context, "example")
            // If you pass an invalid GeneratorParam name, or an inappropriately-typed value,
            // you will fail at Halide compilation time.
            .set_generator_param("compiletime_factor", 2.5f)
            // GeneratorParams that use enumerated types must set the values via string,
            // since the specific C++ enum type isn't visible to us.
            .set_generator_param("enummy", "foo")
            .generate(1.f)
            .set_schedule_param("vectorize", false)
            .schedule();

        Buffer<int32_t> img = gen.realize(kSize, kSize, 3);
        verify(img, 2.5f, 1, 3);
    }

    printf("Success!\n");
    return 0;
}
