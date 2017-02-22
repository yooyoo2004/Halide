#include "Halide.h"

using Halide::Buffer;

namespace {

template<typename Type, int size = 32>
Buffer<Type> make_image() {
    Buffer<Type> im(size, size, 3);
    for (int x = 0; x < size; x++) {
        for (int y = 0; y < size; y++) {
            for (int c = 0; c < 3; c++) {
                im(x, y, c) = static_cast<Type>(x + y + c);
            }
        }
    }
    return im;
}

class StubUser : public Halide::Generator<StubUser> {
public:
    GeneratorParam<int32_t> int_arg{ "int_arg", 33 };

    Input<Buffer<uint8_t>> input{ "input", 3 };
    Output<Buffer<uint8_t>> calculated_output{"calculated_output" };
    Output<Buffer<float>> float32_buffer_output{"float32_buffer_output" };
    Output<Buffer<int32_t>> int32_buffer_output{"int32_buffer_output" };

    void generate() {
        Buffer<uint8_t> constant_image = make_image<uint8_t>();

        // Create an Invoker for the StubTest Generator.
        stub = Invoker(this, "stub_test")
                    // Optionally set GeneratorParams before calling generate(). 
                    // Bad names and/or incompatible types will assert.
                    .set_generator_param("untyped_buffer_output_type", int32_buffer_output.type())
                    // Pass the Inputs, in order, to Invoker's generate() method.
                    // This sets the Input<> values correctly. If you pass the wrong number of Inputs,
                    // or the wrong type for a given Input (e.g. int where a float is expected),
                    // Halide-compile-fail.
                    .generate(
                        constant_image,                  // typed_buffer_input
                        input,                           // untyped_buffer_input
                        input,                           // simple_input
                        std::vector<Func>{ input },      // array_input
                        1.234f,                          // float_arg
                        std::vector<int>{ int_arg }      // int_arg
                    );

        // Get outputs via index or name. If the output's name or index is
        // bad, Halide-compile-fail. If the output can't be assigned to the LHS
        // (e.g. Func f = Output<Func[]>), Halide-compile-fail (or sometimes C++-compile-fail).
        Func simple_output = stub[0];                           // or ["simple_output"]
        Func tuple_output = stub["tuple_output"];               // or [1]
        std::vector<Func> array_output = stub["array_output"];  // or [2]

        // TODO: there is no special path for a single-Output coercing without [] usage, e.g.
        // if "simple_output" was the only Output, we couldn't just do
        //    Func simple_output = stub;
        // but instead we must use
        //    Func simple_output = stub[0];  // or ["simple_output"]
        // We could probably make this work, but IMHO requiring [0] seems minor and 
        // straightforward. Thoughts/

        // TODO: can't use results directly, e.g.
        //    Expr f = stub["simple_output"](x, y, c);       // nope
        // Of course, an explicit cast would work:
        //    Expr f = Func(stub["simple_output"])(x, y, c); // OK
        // We could add overloads to make this work. Do we want to?

        const float kOffset = 2.f;
        calculated_output(x, y, c) = cast<uint8_t>(tuple_output(x, y, c)[1] + kOffset);

        // Output<Buffer> (rather than Output<Func>) can only be assigned to 
        // another Output<Buffer> (or Realized, if JITing); this is useful mainly 
        // to set stride (etc) constraints on the Output.
        float32_buffer_output = stub["typed_buffer_output"];
        int32_buffer_output = stub["untyped_buffer_output"];
    }

    void schedule() {
        stub
            // Optionally set ScheduleParams before calling schedule(). 
            // Bad names and/or incompatible types will assert.
            .set_schedule_param("vectorize", true)
            .set_schedule_param("intermediate_level", LoopLevel(calculated_output, Var("y")))
            .schedule();
    }

private:
    Var x{"x"}, y{"y"}, c{"c"};
    Invoker stub;
};

HALIDE_REGISTER_GENERATOR(StubUser, "stubuser")

}  // namespace
