#include "Halide.h"

#include "stubtest.stub.h"

using namespace Halide;
using StubNS1::StubNS2::StubTest;

const int kSize = 32;

Var x, y, c;

template<typename Type>
Buffer<Type> make_image(int extra) {
    Buffer<Type> im(kSize, kSize, 3);
    im.for_each_element([&im, extra](int x, int y, int c) {
        im(x, y, c) = static_cast<Type>(x + y + c + extra);
    });
    return im;
}

template<typename InputType, typename OutputType>
void verify(const Buffer<InputType> &input, float float_arg, int int_arg, const Buffer<OutputType> &output) {
    if (input.width() != output.width() ||
        input.height() != output.height()) {
        fprintf(stderr, "size mismatch\n");
        exit(-1);
    }
    int channels = std::max(1, std::min(input.channels(), output.channels()));
    for (int x = 0; x < output.width(); x++) {
        for (int y = 0; y < output.height(); y++) {
            for (int c = 0; c < channels; c++) {
                const OutputType expected = static_cast<OutputType>(input(x, y, c) * float_arg + int_arg);
                const OutputType actual = output(x, y, c);
                if (expected != actual) {
                    fprintf(stderr, "img[%d, %d, %d] = %f, expected %f\n", x, y, c, (double)actual, (double)expected);
                    exit(-1);
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    constexpr int kArrayCount = 2;

    auto context = JITGeneratorContext(get_target_from_environment());
    Buffer<uint8_t> buffer_input = make_image<uint8_t>(0);
    Buffer<float> simple_input = make_image<float>(0);
    Buffer<float> array_input[kArrayCount] = {
        make_image<float>(0),
        make_image<float>(1)
    };
    int int_args[2] = { 33, 66 };

    {
        // First, let's test using the custom-generated Stub for this Generator.
        auto gen = StubTest(context);
        gen.generate(buffer_input,  // typed_buffer_input
                     buffer_input,  // untyped_buffer_input
                     Func(simple_input),
                     { Func(array_input[0]), Func(array_input[1]) },
                     1.25f,
                     { int_args[0], int_args[1] });

        // This generator defaults intermediate_level to "undefined",
        // so we *must* specify something for it (else we'll crater at
        // Halide compile time). Since we've called generate(), the outputs in the Stub
        // are valid, so we can examine them to construct a LoopLevel:
        Func tuple_output = gen.tuple_output;
        gen.set_intermediate_level(LoopLevel(tuple_output, tuple_output.args().at(1)));

        gen.schedule();

        Buffer<float> s0 = gen.simple_output.realize(kSize, kSize, 3, gen.get_target());
        verify(array_input[0], 1.f, 0, s0);

        Realization f = gen.tuple_output.realize(kSize, kSize, 3, gen.get_target());
        // Explicitly cast Buffer<> -> Buffer<float> to satisfy verify() type inference
        Buffer<float> f0 = f[0];
        Buffer<float> f1 = f[1];
        verify(array_input[0], 1.25f, 0, f0);
        verify(array_input[0], 1.25f, 33, f1);

        for (int i = 0; i < kArrayCount; ++i) {
            Func f = gen.array_output[i];
            Buffer<int16_t> g0 = f.realize(kSize, kSize, gen.get_target());
            verify(array_input[i], 1.0f, int_args[i], g0);
        }

        Buffer<float> b0 = gen.typed_buffer_output.realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 0, b0);

        Buffer<float> b1 = gen.untyped_buffer_output.realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 0, b1);

        Buffer<uint8_t> b2 = gen.static_compiled_buffer_output.realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 42, b2);
    }

    {
        // Now, let's do the same test using the generic GeneratorStub variant
        // (which doesn't use any of the customization from stubtest.stub.h).
        // First, create the stub via the registry name:
        auto gen = GeneratorStub(context, "StubNS1::StubNS2::StubTest");

        // Call generate() in the same way, adding explicit vector-ctor typing where needed.
        gen.generate(buffer_input,
                     buffer_input,
                     Func(simple_input),
                     std::vector<Func>{ Func(array_input[0]), Func(array_input[1]) },
                     1.25f,
                     std::vector<Expr>{ int_args[0], int_args[1] });

        // Accessing outputs is done via the [] operator, using the name of the output.
        // (If you ask for an output that doesn't exist, you'll fail at Halide compilation time.)
        Func tuple_output = gen["tuple_output"];

        // ScheduleParams are set by string; if no such ScheduleParam exists,
        // or the type we are passing for it is inappropriate, you'll fail at Halide compilation time.
        gen.set_schedule_param("intermediate_level", LoopLevel(tuple_output, tuple_output.args().at(1)));

        gen.schedule();

        // Aside from the subscripting, the rest of the code is basically the
        // same as above.
        Buffer<float> s0 = gen["simple_output"].realize(kSize, kSize, 3);
        verify(array_input[0], 1.f, 0, s0);

        Realization f = gen["tuple_output"].realize(kSize, kSize, 3);
        // Explicitly cast Buffer<> -> Buffer<float> to satisfy verify() type inference
        Buffer<float> f0 = f[0];
        Buffer<float> f1 = f[1];
        verify(array_input[0], 1.25f, 0, f0);
        verify(array_input[0], 1.25f, 33, f1);

        for (int i = 0; i < kArrayCount; ++i) {
            Buffer<int16_t> g0 = gen["array_output"][i].realize(kSize, kSize);
            verify(array_input[i], 1.0f, int_args[i], g0);
        }

        Buffer<float> b0 = gen["typed_buffer_output"].realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 0, b0);

        Buffer<float> b1 = gen["untyped_buffer_output"].realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 0, b1);

        Buffer<uint8_t> b2 = gen["static_compiled_buffer_output"].realize(kSize, kSize, 3);
        verify(buffer_input, 1.f, 42, b2);
    }

    printf("Success!\n");
    return 0;
}
