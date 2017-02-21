#include "Halide.h"

using Halide::Argument;
using Halide::Expr;
using Halide::Func;
using Halide::Buffer;
using Halide::Invoker;
using Halide::JITGeneratorContext;
using Halide::LoopLevel;
using Halide::Var;

const int kSize = 32;

Halide::Var x, y, c;

template<typename Type>
Buffer<Type> make_image(int extra) {
    Buffer<Type> im(kSize, kSize, 3);
    for (int x = 0; x < kSize; x++) {
        for (int y = 0; y < kSize; y++) {
            for (int c = 0; c < 3; c++) {
                im(x, y, c) = static_cast<Type>(x + y + c + extra);
            }
        }
    }
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

    Buffer<uint8_t> buffer_input = make_image<uint8_t>(0);
    Buffer<float> simple_input = make_image<float>(0);
    Buffer<float> array_input[kArrayCount] = {
        make_image<float>(0),
        make_image<float>(1)
    };

    std::vector<int> int_args = { 33, 66 };

    // Create Invoker using default GeneratorParams.
    auto context = JITGeneratorContext(Halide::get_target_from_environment());
    auto invoker = Invoker(context, "stub_test");

    // Pass the Inputs, in order, to Invoker's generate() method.
    // This sets the Input<> values correctly. If you pass the wrong number of Inputs,
    // or the wrong type for a given Input (e.g. int where a float is expected),
    // Halide-compile-fail.
    invoker.generate(
        buffer_input,  // typed_buffer_input
        buffer_input,  // untyped_buffer_input
        Func(simple_input),
        // Note that we can't infer the type needed here, so we must explicitly use a vector<Func> ctor
        std::vector<Func>{ Func(array_input[0]), Func(array_input[1]) },
        1.25f,
        int_args
    );

    Func tuple_output = invoker["tuple_output"];

    // This generator defaults intermediate_level to "undefined",
    // so we *must* specify something for it (else we'll crater at
    // Halide compile time). We'll use this:
    // ...but any of the following would also be OK:
    // intermediate_level = LoopLevel::root();
    // intermediate_level = LoopLevel(gen.tuple_output, Var("x"));
    // intermediate_level = LoopLevel(gen.tuple_output, Var("c"));
    auto intermediate_level = LoopLevel(tuple_output, tuple_output.args().at(1));
    invoker.set_schedule_param("intermediate_level", intermediate_level).schedule();

    Halide::Realization simple_output_realized = invoker["simple_output"].realize(kSize, kSize, 3);
    Buffer<float> s0 = simple_output_realized;
    verify(array_input[0], 1.f, 0, s0);

    Halide::Realization tuple_output_realized = invoker["tuple_output"].realize(kSize, kSize, 3);
    Buffer<float> f0 = tuple_output_realized[0];
    Buffer<float> f1 = tuple_output_realized[1];
    verify(array_input[0], 1.25f, 0, f0);
    verify(array_input[0], 1.25f, 33, f1);

    for (int i = 0; i < kArrayCount; ++i) {
        Halide::Realization array_output_realized = invoker["array_output"][i].realize(kSize, kSize);
        Buffer<int16_t> g0 = array_output_realized;
        verify(array_input[i], 1.0f, int_args[i], g0);
    }

    Halide::Realization typed_buffer_output_realized = invoker["typed_buffer_output"].realize(kSize, kSize, 3);
    Buffer<float> b0 = typed_buffer_output_realized;
    verify(buffer_input, 1.f, 0, b0);

    Halide::Realization untyped_buffer_output_realized = invoker["untyped_buffer_output"].realize(kSize, kSize, 3);
    Buffer<float> b1 = untyped_buffer_output_realized;
    verify(buffer_input, 1.f, 0, b1);

    Halide::Realization static_compiled_buffer_output_realized = invoker["static_compiled_buffer_output"].realize(kSize, kSize, 3);
    Buffer<uint8_t> b2 = static_compiled_buffer_output_realized;
    verify(buffer_input, 1.f, 42, b2);

    printf("Success!\n");
    return 0;
}
