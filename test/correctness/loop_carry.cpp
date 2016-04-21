#include "Halide.h"

using namespace Halide;
using namespace Halide::Internal;

void dump_asm(Func f) {
    Target t;
    t.from_string("host-no_runtime-no_asserts-no_bounds_query");
    f.compile_to_assembly("/dev/stdout", {}, t);
}

class Stats : public IRVisitor {

    using IRVisitor::visit;

    std::set<std::string> scratch_bufs;
    void visit(const Allocate *op) {
        const int64_t *const_extent = op->extents.size() == 1 ? as_const_int(op->extents[0]) : nullptr;
        if (op->name[0] == 'c' && const_extent) {
            scratch_allocs++;
            scratch_bytes += (int)(*const_extent) * op->type.bytes();
            scratch_bufs.insert(op->name);
            IRVisitor::visit(op);
            scratch_bufs.erase(op->name);
        } else {
            IRVisitor::visit(op);
        }
    }

    bool record_loads = false;
    void visit(const Load *op) {
        if (record_loads) {
            if (scratch_bufs.count(op->name)) {
                scratch_loads++;
            } else {
                new_loads++;
            }
        }
        IRVisitor::visit(op);
    }

    void visit(const For *op) {
        if (op->name == var) {
            record_loads = true;
            IRVisitor::visit(op);
            record_loads = false;
        } else {
            IRVisitor::visit(op);
        }
    }

public:
    std::string var;
    int new_loads = 0;
    int scratch_loads = 0;
    int scratch_allocs = 0;
    int scratch_bytes = 0;
};

void validate(Func f, int new_loads, int scratch_loads, int scratch_allocs, int scratch_bytes) {
    Module m = f.compile_to_module({});
    // If loop carry is not enabled for this target, run it manually:
    m.functions[0].body = loop_carry(m.functions[0].body);
    Stats stats;
    stats.var = f.name() + ".s0." + f.args()[0].name();
    m.functions[0].body.accept(&stats);
    if (stats.new_loads != new_loads ||
        stats.scratch_loads != scratch_loads ||
        stats.scratch_allocs != scratch_allocs ||
        stats.scratch_bytes != scratch_bytes) {
        std::cerr << m.functions[0].body
                  << "Expected vs actual:\n"
                  << "  non-scratch loads: " << new_loads << " vs " << stats.new_loads << "\n"
                  << "  scratch loads: " << scratch_loads << " vs " << stats.scratch_loads << "\n"
                  << "  scratch allocs: " << scratch_allocs << " vs " << stats.scratch_allocs << "\n"
                  << "  scratch bytes: " << scratch_bytes << " vs " << stats.scratch_bytes << "\n";
        exit(-1);
    }
}

template<typename T>
void check_equal(Image<T> im1, Image<T> im2) {
    for (int y = 0; y < im1.height(); y++) {
        for (int x = 0; x < im1.width(); x++) {
            if (im1(x, y) != im2(x, y)) {
                std::cerr << "At " << x << ", " << y
                          << " im1 = " << im1(x, y)
                          << " im2 = " << im2(x, y) << "\n";
                exit(-1);
            }
        }
    }
}

template<typename T>
void check_equal(Func f1, Func f2) {
    Image<T> im1 = f1.realize(100, 100);
    Image<T> im2 = f2.realize(100, 100);
    check_equal(im1, im2);
}

int main(int argc, char **argv) {
    {
        Func f, g;
        Var x, y;
        f(x, y) = x % 17 + y % 3;
        g(x, y) = f(x-1, y) + f(x, y) + f(x+1, y);

        f.compute_root();
        validate(g, 1, 5, 1, 4*3);

        Func ref_f, ref_g;
        ref_f(x, y) = x % 17 + y % 3;
        ref_g(x, y) = ref_f(x-1, y) + ref_f(x, y) + ref_f(x+1, y);

        check_equal<int>(g, ref_g);
    }


    {
        // Check it works with whole vectors
        Func f, g;
        Var x, y;
        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f(x-4, y) + f(x, y) + f(x+4, y);

        g.vectorize(x, 4);

        g.realize(100, 100);
    }

    {
        Func f, g, h;
        Var x, y;
        f(x, y) = x + y;
        h(x, y) = x + y;
        g(x, y) = f(x-1, y) + f(x, y) + f(x+1, y) + h(x, y);

        f.compute_root();
        h.compute_at(g, x);

        g.realize(100, 100);
    }

    {
        Func f, g;
        Var x, y;
        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f(min(100, x-1), y) + f(min(100, x), y) + f(min(100, x+1), y);

        g.realize(100, 100);
    }

    {
        Func f, g;
        Var x, y;
        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f(clamp(f(x-1, y), 0, 100), y) + f(clamp(f(x, y), 0, 100), y);

        g.realize(100, 100);
    }

    {
        // A case where the index is lifted out into a let
        Func f, g;
        Var x, y;
        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f((x-y*2), y) + f((x-y*2)+1, y) + f((x-y*2)+2, y);

        g.realize(100, 100);
    }

    {
        // A case where the index and a load are both lifted out into a let
        Func f, g;
        Var x, y;
        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f((x-y*2), y) + f((x-y*2)+1, y) + f((x-y*2)+2, y) + f((x-y*2)+2, y);

        g.realize(100, 100);
    }

    if (0) {
        // A case where there's a combinatorially large Expr going on.
        Func f, g;
        Var x, y;

        Expr idx = 0;
        for (int i = 0; i < 20; i++) {
            idx += y;
            idx *= idx;
        }

        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f((x-idx), y) + f((x-idx)+1, y) + f((x-idx)+2, y);

        // Don't send it to LLVM. LLVM has its own problems with these Exprs.
        g.compile_to_module({}, "g");
    }

    {
        // A case with an inner loop.
        Func f, g;
        Var x, y, c;

        f(x, y) = x + y;
        f.compute_root();
        g(c, x, y) = f(x, y) + f(x+1, y) + f(x+2, y) + c;
        g.bound(c, 0, 3).unroll(c).unroll(x, 2);

        g.realize(3, 100, 100);
    }


    {
        // A case with weirdly-spaced taps
        Func f, g;
        Var x, y;

        f(x, y) = x + y;
        f.compute_root();
        g(x, y) = f(x, y) + f(x+1, y) + f(x+3, y);

        g.realize(100, 100);
    }

    {
        // A case with far too many entries to keep around
        Func f, g, h;
        Var x, y, c;

        f(c, x, y) = c + x + y;
        f.compute_root();

        g(c, x, y) = f(c, x-2, y) + f(c, x-1, y) + f(c, x, y) + f(c, x+1, y) + f(c, x+2, y);
        h(c, x, y) = g(c, x, y-2) + g(c, x, y-1) + g(c, x, y) + g(c, x, y+1) + g(c, x, y+2) + f(c, x-3, y);

        h.bound(c, 0, 4).vectorize(c);

        h.realize(4, 100, 100);
    }

    if (get_jit_target_from_environment().has_gpu_feature()) {
        // Reusing values from local memory on the GPU is much better
        // than reloading from shared or global.
        Func f, g, h;
        Var x, y;

        f(x, y) = cast<float>(x + y);
        f.compute_root();

        g(x, y) = f(x-2, y) + f(x-1, y) + f(x, y) + f(x+1, y) + f(x+2, y);
        Var xo, xi;
        g.split(x, xo, xi, 16).gpu_tile(xo, y, 8, 8);

        g.realize(160, 100);
    }


    return 0;
}
