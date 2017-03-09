#include "Halide.h"

using namespace Halide;

constexpr int kSize = 32;

void verify(const Buffer<int32_t> &img, float compiletime_factor, float runtime_factor, int channels) {
    img.for_each_element([=](int x, int y, int c) {
        int expected = (int32_t)(compiletime_factor * runtime_factor * c * (x > y ? x : y));
        int actual = img(x, y, c);
        assert(expected == actual);
    });
}

class Example : public Halide::Generator<Example> {
public:
    enum class SomeEnum { Foo, Bar };

    GeneratorParam<float> compiletime_factor{ "compiletime_factor", 1, 0, 100 };
    GeneratorParam<int> channels{ "channels", 3 };
    GeneratorParam<SomeEnum> enummy{ "enummy",
                                     SomeEnum::Foo,
                                     { { "foo", SomeEnum::Foo }, { "bar", SomeEnum::Bar } } };

    ScheduleParam<bool> vectorize{ "vectorize", true };

    Input<float> runtime_factor{ "runtime_factor", 1.0 };

    Output<Func> output{ "output", Int(32), 3 };

    void generate() {
        Func f;
        f(x, y) = max(x, y);
        output(x, y, c) = cast(output.type(), f(x, y) * c * compiletime_factor * runtime_factor);
    }

    void schedule() {
        Func(output).bound(c, 0, channels).reorder(c, x, y).unroll(c);
        if (vectorize) {
            Func(output).vectorize(x, natural_vector_size(output.type()));
        }
    }

private:
    Var x{"x"}, y{"y"}, c{"c"};
};

int main(int argc, char **argv) {
    JITGeneratorContext context(get_target_from_environment());

    {
        // If you have a Generator in a visible translation unit (ie
        // in the same source file, or visible via #include), you can
        // use it directly -- even if it's not registered -- via the generic 
        // GeneratorStub class; just call GeneratorStub::create<GENERATOR_TYPE> to
        // create the stub, followed by generate() and schedule() calls.
        GeneratorStub gen = GeneratorStub::create<Example>(context)
            .set_generator_param("compiletime_factor", 2.5f)
            .set_generator_param("enummy", "foo")
            .generate(1.f)
            .set_schedule_param("vectorize", false)
            .schedule();

        Buffer<int32_t> img = gen.realize(kSize, kSize, 3);
        verify(img, 2.5f, 1, 3);
    }

    {
        // Alternate, equivalent syntax is to call GENERATOR_TYPE::create(),
        // which is a trivial wrapper for the above.
        GeneratorStub gen = Example::create(context)
            .set_generator_param("compiletime_factor", 2.5f)
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
