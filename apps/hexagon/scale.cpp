#include "Halide.h"

using namespace Halide;

Expr u16(Expr x) { return cast<uint16_t>(x); }
Expr u8(Expr x) { return cast<uint8_t>(x); }

int main(int argc, char **argv) {

    Target target = get_target_from_environment();

    Var x("x"), y("y"), c("c");

    // Takes an 8-bit input
    ImageParam input(UInt(8), 3);

    Func input_bounded = BoundaryConditions::repeat_edge(input);

    int radius = 3;

    RDom ry(-radius, 2*radius + 1);

    Func f("f");
    f(x, y, c) = u8(sum(u16(input_bounded(x, y + ry, c)))/(2*radius + 1));
    Func g("g");
    g(x, y, c) = f(x, y, c);

    f.bound(c, 0, 3);

#if 1
    f.compute_root().hexagon(c);  //.vectorize(x, 64);
#else
    f.compute_root().vectorize(x, target.natural_vector_size<uint8_t>());
#endif

    g.compile_to_header("scale.h", {input}, "scale");
    std::stringstream obj;
    obj << "scale-" << argv[1] << ".o";
    g.compile_to_object(obj.str(), {input}, "scale", target);

    return 0;
}