#include "Halide.h"

using namespace Halide;

class CountSelects : public Internal::IRVisitor {
public:
    int count = 0;
private:
    using Internal::IRVisitor::visit;

    void visit(const Internal::Select *op) {
        count++;
        Internal::IRVisitor::visit(op);
    }
};

int main(int argc, char **argv) {

    // Loop iterations that would be no-ops should be trimmed off.
    Func f;
    Var x;
    f(x) = x;
    f(x) += select(x > 10 && x < 20, 1, 0);
    f(x) += select(x < 10, 0, 1);
    f(x) *= select(x > 20 && x < 30, 2, 1);
    f(x) = select(x >= 60 && x <= 100, 100 - f(x), f(x));
    Module m = f.compile_to_module({});

    // There should be no selects after trim_no_ops runs
    CountSelects s;
    m.functions[0].body.accept(&s);
    if (s.count != 0) {
        std::cerr << "There were selects in the lowered code: " << m.functions[0].body << "\n";
        return -1;
    }

    // Also check the output is correct
    Image<int> im = f.realize(100);
    for (int x = 0; x < im.width(); x++) {
        int correct = x;
        correct += (x > 10 && x < 20) ? 1 : 0;
        correct += (x < 10) ? 0 : 1;
        correct *= (x > 20 && x < 30) ? 2 : 1;
        correct = (x >= 60 && x <= 100) ? (100 - correct) : correct;
        if (im(x) != correct) {
            printf("im(%d) = %d instead of %d\n",
                   x, im(x), correct);
            return -1;
        }
    }

    printf("Success!\n");
    return 0;
}
