#include "Solve.h"
#include "Simplify.h"
#include "IRMutator.h"
#include "IREquality.h"
#include "Substitute.h"
#include "CSE.h"
#include "ExprUsesVar.h"

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::pair;
using std::make_pair;

namespace {

/** A mutator that moves all instances of a free variable as far left
 * and as far outermost as possible. See the test cases at the bottom
 * of this file.
 *
 * This mutator substitutes in lets. This means two things:
 * 1) The mutate method must cache partial results
 * 2) Users of this had better immediately run
 * common-subexpression-elimination. Fortunately this isn't a
 * public class, so the only user is in this file.
 */
class SolveExpression : public IRMutator {
public:
    SolveExpression(const string &v, const Scope<Expr> &es) :
        failed(false), var(v), uses_var(false), external_scope(es) {}

    using IRMutator::mutate;

    Expr mutate(Expr e) {
        // If the solve has already failed. Bail out.
        if (failed) {
            return e;
        }

        map<Expr, CacheEntry, ExprCompare>::iterator iter = cache.find(e);
        if (iter == cache.end()) {
            // Not in the cache, call the base class version.
            Expr new_e = IRMutator::mutate(e);
            CacheEntry entry = {new_e, uses_var};
            cache[e] = entry;
            debug(4) << "Rewrote " << e << " -> " << new_e << "\n";
            return new_e;
        } else {
            // Cache hit.
            uses_var = uses_var || iter->second.uses_var;
            debug(4) << "Rewrote " << e << " -> " << iter->second.expr << "\n";
            return iter->second.expr;
        }
    }

    // Has the solve failed.
    bool failed;

private:

    // The variable we're solving for.
    string var;

    // Whether or not the just-mutated expression uses the variable.
    bool uses_var;

    // A cache of mutated results. Fortunately the mutator is
    // stateless, so we can cache everything.
    struct CacheEntry {
        Expr expr;
        bool uses_var;
    };
    map<Expr, CacheEntry, ExprCompare> cache;

    // Internal lets. Already mutated.
    Scope<CacheEntry> scope;

    // External lets.
    const Scope<Expr> &external_scope;

    // Return the negative of an expr. Does some eager simplification
    // to avoid injecting pointless -1s.
    Expr negate(Expr e) {
        const Mul *mul = e.as<Mul>();
        if (mul && is_const(mul->b)) {
            return mul->a * simplify(-1*mul->b);
        } else {
            return e * -1;
        }
    }


    // The invariant here is that for all the nodes we peephole
    // recognize in each visitor, recursively calling mutate has
    // already moved the part that contains the variable to the left,
    // so the right of the subexpression can be considered a
    // constant. The mutator must preserve this property or set the
    // flag "failed" to true.
    using IRMutator::visit;

    // Admit defeat. Isolated in a method for ease of debugging.
    void fail(Expr e) {
        debug(3) << "Failed to solve: " << e << "\n";
        failed = true;
    }

    void visit(const Add *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr a = mutate(op->a);
        bool a_uses_var = uses_var;

        uses_var = false;
        Expr b = mutate(op->b);
        bool b_uses_var = uses_var;
        uses_var = old_uses_var || a_uses_var || b_uses_var;

        if (b_uses_var && !a_uses_var) {
            std::swap(a, b);
            std::swap(a_uses_var, b_uses_var);
        }

        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        expr = Expr();

        if (a_uses_var && !b_uses_var) {
            if (sub_a) {
                // (f(x) - a) + b -> f(x) + (b - a)
                expr = mutate(sub_a->a + (b - sub_a->b));
            } else if (add_a) {
                // (f(x) + a) + b -> f(x) + (a + b)
                expr = mutate(add_a->a + (add_a->b + b));
            }
        } else if (a_uses_var && b_uses_var) {
            if (equal(a, b)) {
                expr = mutate(a*2);
            } else if (add_a) {
                // (f(x) + a) + g(x) -> (f(x) + g(x)) + a
                expr = mutate((add_a->a + b) + add_a->b);
            } else if (add_b) {
                // f(x) + (g(x) + a) -> (f(x) + g(x)) + a
                expr = mutate((a + add_b->a) + add_b->b);
            } else if (sub_a) {
                // (f(x) - a) + g(x) -> (f(x) + g(x)) - a
                expr = mutate((sub_a->a + b) - sub_a->b);
            } else if (sub_b) {
                // f(x) + (g(x) - a) -> (f(x) + g(x)) - a
                expr = mutate((a + sub_b->a) - sub_b->b);
            } else if (mul_a && mul_b && equal(mul_a->a, mul_b->a)) {
                // f(x)*a + f(x)*b -> f(x)*(a + b)
                expr = mutate(mul_a->a * (mul_a->b + mul_b->b));
            } else if (mul_a && mul_b && equal(mul_a->b, mul_b->b)) {
                // f(x)*a + g(x)*a -> (f(x) + g(x))*a;
                expr = mutate(mul_a->a + mul_b->a) * mul_a->b;
            } else if (mul_a && equal(mul_a->a, b)) {
                // f(x)*a + f(x) -> f(x) * (a + 1)
                expr = mutate(b * (mul_a->b + 1));
            } else if (mul_b && equal(mul_b->a, a)) {
                // f(x) + f(x)*a -> f(x) * (a + 1)
                expr = mutate(a * (mul_b->b + 1));
            } else {
                fail(a + b);
            }
        } else {
            // Do some constant-folding
            if (is_const(a) && is_const(b)) {
                expr = simplify(a + b);
            }
        }

        if (!expr.defined()) {
            if (a.same_as(op->a) && b.same_as(op->b)) {
                expr = op;
            } else {
                expr = a + b;
            }
        }

    }

    void visit(const Sub *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr a = mutate(op->a);
        bool a_uses_var = uses_var;

        uses_var = false;
        Expr b = mutate(op->b);
        bool b_uses_var = uses_var;
        uses_var = old_uses_var || a_uses_var || b_uses_var;

        const Add *add_a = a.as<Add>();
        const Add *add_b = b.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Sub *sub_b = b.as<Sub>();
        const Mul *mul_a = a.as<Mul>();
        const Mul *mul_b = b.as<Mul>();

        expr = Expr();

        if (a_uses_var && !b_uses_var) {
            if (sub_a) {
                // (f(x) - a) - b -> f(x) - (a + b)
                expr = mutate(sub_a->a - (sub_a->b + b));
            } else if (add_a) {
                // (f(x) + a) - b -> f(x) + (a - b)
                expr = mutate(add_a->a + (add_a->b - b));
            }
        } else if (b_uses_var && !a_uses_var) {
            if (sub_b) {
                // a - (f(x) - b) -> -f(x) + (a + b)
                expr = mutate(negate(sub_b->a) + (a + sub_b->b));
            } else if (add_b) {
                // a - (f(x) + b) -> -f(x) + (a - b)
                expr = mutate(negate(add_b->a) + (a - add_b->b));
            } else {
                expr = mutate(negate(b) + a);
            }
        } else if (a_uses_var && b_uses_var) {
            if (add_a) {
                // (f(x) + a) - g(x) -> (f(x) - g(x)) + a
                expr = mutate(add_a->a - b + add_a->b);
            } else if (add_b) {
                // f(x) - (g(x) + a) -> (f(x) - g(x)) - a
                expr = mutate(a - add_b->a - add_b->b);
            } else if (sub_a) {
                // (f(x) - a) - g(x) -> (f(x) - g(x)) - a
                expr = mutate(sub_a->a - b - sub_a->b);
            } else if (sub_b) {
                // f(x) - (g(x) - a) -> (f(x) - g(x)) - a
                expr = mutate(a - sub_b->a - sub_b->b);
            } else if (mul_a && mul_b && equal(mul_a->a, mul_b->a)) {
                // f(x)*a - f(x)*b -> f(x)*(a - b)
                expr = mutate(mul_a->a * (mul_a->b - mul_b->b));
            } else if (mul_a && mul_b && equal(mul_a->b, mul_b->b)) {
                // f(x)*a - g(x)*a -> (f(x) - g(x)*a);
                expr = mutate((mul_a->a - mul_b->a) * mul_a->b);
            } else {
                fail(a - b);
            }
        } else {
            // Do some constant-folding
            if (is_const(a) && is_const(b)) {
                expr = simplify(a - b);
            }
        }

        if (!expr.defined()) {
            if (a.same_as(op->a) && b.same_as(op->b)) {
                expr = op;
            } else {
                expr = a - b;
            }
        }
    }

    void visit(const Mul *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr a = mutate(op->a);
        bool a_uses_var = uses_var;

        uses_var = false;
        Expr b = mutate(op->b);
        bool b_uses_var = uses_var;
        uses_var = old_uses_var || a_uses_var || b_uses_var;

        const Add *add_a = a.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Mul *mul_a = a.as<Mul>();

        if (b_uses_var && !a_uses_var) {
            std::swap(a, b);
            std::swap(a_uses_var, b_uses_var);
        }

        expr = Expr();
        if (a_uses_var && !b_uses_var) {
            if (add_a) {
                // (f(x) + a) * b -> f(x) * b + a * b
                expr = mutate(add_a->a * b + add_a->b * b);
            } else if (sub_a) {
                // (f(x) - a) * b -> f(x) * b - a * b
                expr = mutate(sub_a->a * b - sub_a->b * b);
            } else if (mul_a) {
                // (f(x) * a) * b -> f(x) * (a * b)
                expr = mutate(mul_a->a * (mul_a->b * b));
            }
        } else if (a_uses_var && b_uses_var) {
            // It's a quadratic. We could continue but this is
            // unlikely to ever occur. Code will be added here as
            // these cases actually pop up.
            fail(a * b);
        } else {
            // Do some constant-folding
            if (is_const(a) && is_const(b)) {
                expr = simplify(a * b);
            }
        }

        if (!expr.defined()) {
            if (a.same_as(op->a) && b.same_as(op->b)) {
                expr = op;
            } else {
                expr = a * b;
            }
        }
    }

    void visit(const Call *op) {
        // Ignore likely intrinsics
        if (op->name == Call::likely && op->call_type == Call::Intrinsic) {
            expr = mutate(op->args[0]);
        } else {
            IRMutator::visit(op);
        }
    }

    template<typename T>
    void visit_commutative_op(const T *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr a = mutate(op->a);
        bool a_uses_var = uses_var;

        uses_var = false;
        Expr b = mutate(op->b);
        bool b_uses_var = uses_var;
        uses_var = old_uses_var || a_uses_var || b_uses_var;

        if (b_uses_var && !a_uses_var) {
            std::swap(a, b);
        } else if (a_uses_var && b_uses_var) {
            fail(T::make(a, b));
        }

        if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = T::make(a, b);
        }
    }

    void visit(const Min *op) {
        visit_commutative_op(op);
    }

    void visit(const Max *op) {
        visit_commutative_op(op);
    }

    void visit(const Or *op) {
        visit_commutative_op(op);
    }

    void visit(const And *op) {
        visit_commutative_op(op);
    }

    template<typename Cmp, typename Opp>
    void visit_cmp(const Cmp *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr a = mutate(op->a);
        bool a_uses_var = uses_var;

        uses_var = false;
        Expr b = mutate(op->b);
        bool b_uses_var = uses_var;
        uses_var = old_uses_var || a_uses_var || b_uses_var;

        if (b_uses_var && !a_uses_var) {
            expr = mutate(Opp::make(b, a));
            return;
        }

        const Add *add_a = a.as<Add>();
        const Sub *sub_a = a.as<Sub>();
        const Mul *mul_a = a.as<Mul>();

        bool is_eq = Expr(op).as<EQ>() != NULL;
        bool is_ne = Expr(op).as<NE>() != NULL;
        bool is_lt = Expr(op).as<LT>() != NULL;
        bool is_le = Expr(op).as<LE>() != NULL;
        bool is_ge = Expr(op).as<GE>() != NULL;
        bool is_gt = Expr(op).as<GT>() != NULL;

        expr = Expr();

        if (a_uses_var && !b_uses_var) {
            // We have f(x) < y. Try to unwrap f(x)
            if (add_a) {
                // f(x) + b < c -> f(x) < c - b
                expr = mutate(Cmp::make(add_a->a, (b - add_a->b)));
            } else if (sub_a) {
                // f(x) - b < c -> f(x) < c + b
                expr = mutate(Cmp::make(sub_a->a, (b + sub_a->b)));
            } else if (mul_a) {
                if (op->type.is_float()) {
                    // f(x) * b == c -> f(x) == c / b
                    if (is_eq || is_ne || is_positive_const(mul_a->b)) {
                        expr = mutate(Cmp::make(mul_a->a, (b / mul_a->b)));
                    } else if (is_negative_const(mul_a->b)) {
                        expr = mutate(Opp::make(mul_a->a, (b / mul_a->b)));
                    }
                } else {
                    // Don't use operator/ and operator % to sneak
                    // past the division-by-zero check. We'll only
                    // actually use these when mul_a->b is a positive
                    // or negative constant.
                    Expr div = Div::make(b, mul_a->b);
                    Expr rem = Mod::make(b, mul_a->b);
                    if (is_eq) {
                        // f(x) * c == b -> f(x) == b/c && b%c == 0
                        expr = mutate((mul_a->a == div) && (rem == 0));
                    } else if (is_ne) {
                        // f(x) * c != b -> f(x) != b/c || b%c != 0
                        expr = mutate((mul_a->a != div) || (rem != 0));
                    } else if (is_le) {
                        if (is_positive_const(mul_a->b)) {
                            expr = mutate(mul_a->a <= div);
                        } else if (is_negative_const(mul_a->b)) {
                            expr = mutate(mul_a->a >= div);
                        } else {
                            fail(Cmp::make(a, b));
                        }
                    } else if (is_lt) {
                        if (is_positive_const(mul_a->b)) {
                            expr = mutate(mul_a->a < (b + (mul_a->b - 1)) / mul_a->b);
                        } else if (is_negative_const(mul_a->b)) {
                            expr = mutate(mul_a->a > (b - (mul_a->b + 1)) / mul_a->b);
                        } else {
                            fail(Cmp::make(a, b));
                        }
                    } else if (is_gt) {
                        if (is_positive_const(mul_a->b)) {
                            expr = mutate(mul_a->a > div);
                        } else if (is_negative_const(mul_a->b)) {
                            expr = mutate(mul_a->a < div);
                        } else {
                            fail(Cmp::make(a, b));
                        }
                    } else if (is_ge) {
                        if (is_positive_const(mul_a->b)) {
                            expr = mutate(mul_a->a >= (b + (mul_a->b - 1)) / mul_a->b);
                        } else if (is_negative_const(mul_a->b)) {
                            expr = mutate(mul_a->a <= (b - (mul_a->b + 1)) / mul_a->b);
                        } else {
                            fail(Cmp::make(a, b));
                        }
                    }
                }
            }
        } else if (a_uses_var && b_uses_var && a.type().is_int() && a.type().bits >= 32) {
            // Convert to f(x) - g(x) == 0 and let the subtract mutator clean up.
            // Only safe if the type is not subject to overflow.
            expr = mutate(Cmp::make(a - b, make_zero(a.type())));
        }

        if (!expr.defined()) {
            if (a.same_as(op->a) && b.same_as(op->b)) {
                expr = op;
            } else {
                expr = Cmp::make(a, b);
            }
        }
    }

    void visit(const LT *op) {
        visit_cmp<LT, GT>(op);
    }

    void visit(const LE *op) {
        visit_cmp<LE, GE>(op);
    }

    void visit(const GE *op) {
        visit_cmp<GE, LE>(op);
    }

    void visit(const GT *op) {
        visit_cmp<GT, LT>(op);
    }

    void visit(const EQ *op) {
        visit_cmp<EQ, EQ>(op);
    }

    void visit(const NE *op) {
        visit_cmp<NE, NE>(op);
    }

    void visit(const Variable *op) {
        if (op->name == var) {
            uses_var = true;
            expr = op;
        } else if (scope.contains(op->name)) {
            CacheEntry e = scope.get(op->name);
            expr = e.expr;
            uses_var = uses_var || e.uses_var;
        } else if (external_scope.contains(op->name)) {
            Expr e = external_scope.get(op->name);
            // Expressions in the external scope haven't been solved
            // yet. This will either pull its solution from the cache,
            // or solve it and then put it into the cache.
            expr = mutate(e);
        } else {
            expr = op;
        }
    }

    void visit(const Let *op) {
        bool old_uses_var = uses_var;
        uses_var = false;
        Expr value = mutate(op->value);
        CacheEntry e = {value, uses_var};

        uses_var = old_uses_var;
        scope.push(op->name, e);
        expr = mutate(op->body);
        scope.pop(op->name);
    }

};

Expr pos_inf = Variable::make(Int(32), "pos_inf");
Expr neg_inf = Variable::make(Int(32), "neg_inf");

Expr interval_max(Expr a, Expr b) {
    if (a.same_as(pos_inf) || b.same_as(pos_inf)) {
        return pos_inf;
    } else if (a.same_as(neg_inf)) {
        return b;
    } else if (b.same_as(neg_inf)) {
        return a;
    } else {
        return max(a, b);
    }
}

Expr interval_min(Expr a, Expr b) {
    if (a.same_as(neg_inf) || b.same_as(neg_inf)) {
        return neg_inf;
    } else if (a.same_as(pos_inf)) {
        return b;
    } else if (b.same_as(pos_inf)) {
        return a;
    } else {
        return min(a, b);
    }
}

Interval interval_intersection(Interval ia, Interval ib) {
    return Interval(interval_max(ia.min, ib.min), interval_min(ia.max, ib.max));
}

Interval interval_union(Interval ia, Interval ib) {
    return Interval(interval_min(ia.min, ib.min), interval_max(ia.max, ib.max));
}

class SolveForInterval : public IRVisitor {

    // The var we're solving for
    const string &var;

    // Whether we're trying to make the condition true or false
    bool target = true;

    // Whether we want an outer bound or an inner bound
    bool outer;

    // Track lets expressions. Initially empty.
    Scope<Expr> scope;

    // Lazily populated with solved intervals for boolean sub-expressions.
    map<pair<string, bool>, Interval> solved_vars;

    // Has this expression already been rearranged by solve_expression?
    bool already_solved = false;

    using IRVisitor::visit;

    void fail() {
        if (outer) {
            // If we're looking for an outer bound, then return an infinite interval.
            result = Interval(neg_inf, pos_inf);
        } else {
            // If we're looking for an inner bound, return an empty interval
            result = Interval(pos_inf, neg_inf);
        }
    }

    void visit(const UIntImm *op) {
        internal_assert(op->type.is_bool());
        if ((op->value && target) ||
            (!op->value && !target)) {
            result = Interval(neg_inf, pos_inf);
        } else if ((!op->value && target) ||
                   (op->value && !target)) {
            result = Interval(pos_inf, neg_inf);
        } else {
            fail();
        }
    }

    void visit(const And *op) {
        op->a.accept(this);
        Interval ia = result;
        op->b.accept(this);
        Interval ib = result;
        if (target) {
            result = interval_intersection(ia, ib);
        } else {
            result = interval_union(ia, ib);
        }
    }

    void visit(const Or *op) {
        op->a.accept(this);
        Interval ia = result;
        op->b.accept(this);
        Interval ib = result;
        if (!target) {
            result = interval_intersection(ia, ib);
        } else {
            result = interval_union(ia, ib);
        }
    }

    void visit(const Not *op) {
        target = !target;
        op->a.accept(this);
        target = !target;
    }

    void visit(const Let *op) {
        internal_assert(op->type.is_bool());
        // If it's a bool, we might need to know the intervals over
        // which it's definitely or definitely false. We'll do this
        // lazily and populate a map. See the Variable visitor.
        scope.push(op->name, op->value);
        op->body.accept(this);
        scope.pop(op->name);
        if (result.min.defined() && expr_uses_var(result.min, op->name)) {
            result.min = Let::make(op->name, op->value, result.min);
        }
        if (result.max.defined() && expr_uses_var(result.max, op->name)) {
            result.max = Let::make(op->name, op->value, result.max);
        }
    }

    void visit(const Variable *op) {
        internal_assert(op->type.is_bool());
        if (scope.contains(op->name)) {
            auto key = make_pair(op->name, target);
            auto it = solved_vars.find(key);
            if (it != solved_vars.end()) {
                result = it->second;
            } else {
                scope.get(op->name).accept(this);
                solved_vars[key] = result;
            }
        } else {
            fail();
        }
    }

    void visit(const LT *lt) {
        // Normalize to le
        Expr cond = lt->a <= (lt->b - 1);
        cond.accept(this);
    }

    void visit(const GT *gt) {
        // Normalize to ge
        Expr cond = gt->a >= (gt->b + 1);
        cond.accept(this);
    }

    void visit(const LE *le) {
        const Variable *v = le->a.as<Variable>();
        if (!already_solved) {
            Expr solved = solve_expression(le, var, scope);
            if (!solved.defined()) {
                fail();
            } else {
                already_solved = true;
                solved.accept(this);
                already_solved = false;
            }
        } else if (v && v->name == var) {
            if (target) {
                result = Interval(neg_inf, le->b);
            } else {
                result = Interval(le->b + 1, pos_inf);
            }
        } else if (const Max *max_a = le->a.as<Max>()) {
            // Rewrite (max(a, b) <= c) <==> (a <= c && (b <= c || a >= b))
            // Also allow re-solving the new equations.
            Expr a = max_a->a, b = max_a->b, c = le->b;
            Expr cond = (a <= c) && (b <= c || a >= b);
            already_solved = false;
            cond.accept(this);
            already_solved = true;
        } else if (const Min *min_a = le->a.as<Min>()) {
            // Rewrite (min(a, b) <= c) <==> (a <= c || (b <= c && a >= b))
            Expr a = min_a->a, b = min_a->b, c = le->b;
            Expr cond = (a <= c) || (b <= c && a >= b);
            already_solved = false;
            cond.accept(this);
            already_solved = true;
        } else {
            fail();
        }
    }

    void visit(const GE *ge) {
        const Variable *v = ge->a.as<Variable>();
        if (!already_solved) {
            Expr solved = solve_expression(ge, var, scope);
            if (!solved.defined()) {
                fail();
            } else {
                already_solved = true;
                solved.accept(this);
                already_solved = false;
            }
        } else if (v && v->name == var) {
            if (target) {
                result = Interval(ge->b, pos_inf);
            } else {
                result = Interval(neg_inf, ge->b - 1);
            }
        } else if (const Max *max_a = ge->a.as<Max>()) {
            // Rewrite (max(a, b) >= c) <==> (a >= c || (b >= c && a <= b))
            // Also allow re-solving the new equations.
            Expr a = max_a->a, b = max_a->b, c = ge->b;
            Expr cond = (a >= c) || (b >= c && a <= b);
            already_solved = false;
            cond.accept(this);
            already_solved = true;
        } else if (const Min *min_a = ge->a.as<Min>()) {
            // Rewrite (min(a, b) >= c) <==> (a >= c && (b >= c || a <= b))
            Expr a = min_a->a, b = min_a->b, c = ge->b;
            Expr cond = (a >= c) && (b >= c || a <= b);
            already_solved = false;
            cond.accept(this);
            already_solved = true;
        } else {
            fail();
        }
    }

    void visit(const EQ *op) {
        fail();
    }

    void visit(const NE *op) {
        fail();
    }

public:
    Interval result;

    SolveForInterval(const string &v, bool o) : var(v), outer(o) {}

};

} // Anonymous namespace

Expr solve_expression(Expr e, const std::string &variable, const Scope<Expr> &scope) {
    SolveExpression solver(variable, scope);
    e = solver.mutate(e);
    if (solver.failed) {
        return Expr();
    } else {
        // The process has expanded lets. Re-collect them.
        return common_subexpression_elimination(e);
    }
}


Interval solve_for_inner_interval(Expr c, const std::string &var) {
    SolveForInterval s(var, false);
    c.accept(&s);
    return s.result;
}

Interval solve_for_outer_interval(Expr c, const std::string &var) {
    SolveForInterval s(var, true);
    c.accept(&s);
    return s.result;
}

bool interval_has_lower_bound(const Interval &i) {
    return !i.min.same_as(neg_inf);
}

bool interval_has_upper_bound(const Interval &i) {
    return !i.max.same_as(pos_inf);
}

bool interval_is_empty(const Interval &i) {
    return i.min.same_as(pos_inf) || i.max.same_as(neg_inf);
}

bool interval_is_everything(const Interval &i) {
    return i.min.same_as(neg_inf) && i.max.same_as(pos_inf);
}


// Testing code

namespace {

void check_solve(Expr a, Expr b) {
    Expr c = solve_expression(a, "x");
    internal_assert(equal(c, b))
        << "Expression: " << a << "\n"
        << " solved to " << c << "\n"
        << " instead of " << b << "\n";
}

void check_interval(Expr a, Interval i, bool outer) {
    Interval result =
        outer ?
        solve_for_outer_interval(a, "x") :
        solve_for_inner_interval(a, "x");
    result.min = simplify(result.min);
    result.max = simplify(result.max);
    internal_assert(equal(result.min, i.min) && equal(result.max, i.max))
        << "Expression " << a << " solved to the interval:\n"
        << "  min: " << result.min << "\n"
        << "  max: " << result.max << "\n"
        << " instead of:\n"
        << "  min: " << i.min << "\n"
        << "  max: " << i.max << "\n";

}

void check_outer_interval(Expr a, Expr min, Expr max) {
    check_interval(a, Interval(min, max), true);
}

void check_inner_interval(Expr a, Expr min, Expr max) {
    check_interval(a, Interval(min, max), false);
}

}

void solve_test() {
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");

    // Check some simple cases
    check_solve(3 - 4*x, x*(-4) + 3);
    check_solve(min(5, x), min(x, 5));
    check_solve(max(5, (5+x)*y), max(x*y + 5*y, 5));
    check_solve(5*y + 3*x == 2, ((x == ((2 - (5*y))/3)) && (((2 - (5*y)) % 3) == 0)));

    // A let statement
    check_solve(Let::make("z", 3 + 5*x, y + z < 8),
          x < (((8 - (3 + y)) + 4)/5));

    // A let statement where the variable gets used twice.
    check_solve(Let::make("z", 3 + 5*x, y + (z + z) < 8),
          x < (((8 - (6 + y)) + 9)/10));

    // Something where we expect a let in the output.
    {
        Expr e = y+1;
        for (int i = 0; i < 10; i++) {
            e *= (e + 1);
        }
        Expr c = solve_expression(x + e < e*e, "x");
        internal_assert(c.as<Let>());
    }

    // Solving inequalities for integers is a pain to get right with
    // all the rounding rules. Check we didn't make a mistake with
    // brute force.
    for (int den = -3; den <= 3; den ++) {
        if (den == 0) continue;
        for (int num = 5; num <= 10; num++) {
            Expr in[] = {x*den < num, x*den <= num, x*den == num, x*den != num, x*den >= num, x*den > num};
            for (int j = 0; j < 6; j++) {
                Expr out = simplify(solve_expression(in[j], "x"));
                for (int i = -10; i < 10; i++) {
                    Expr in_val = substitute("x", i, in[j]);
                    Expr out_val = substitute("x", i, out);
                    in_val = simplify(in_val);
                    out_val = simplify(out_val);
                    internal_assert(equal(in_val, out_val))
                        << "Error: "
                        << in[j] << " is not equivalent to "
                        << out << " when x == " << i << "\n";
                }
            }
        }
    }

    // Check for combinatorial explosion
    Expr e = x + y;
    for (int i = 0; i < 20; i++) {
        e += (e + 1) * y;
    }
    e = solve_expression(e, "x");
    internal_assert(e.defined());

    // Check some things that we don't expect to work.

    // Quadratics:
    internal_assert(!solve_expression(x*x < 4, "x").defined());

    // Multiplication by things of unknown sign:
    internal_assert(!solve_expression(x*y < 4, "x").defined());

    // Function calls and cast nodes don't get inverted, but the bit
    // containing x still gets moved leftwards.
    check_solve(4.0f > sqrt(x), sqrt(x) < 4.0f);

    // Now test solving for an interval
    check_inner_interval(x > 0, 1, pos_inf);
    check_inner_interval(x < 100, neg_inf, 99);
    check_outer_interval(x > 0 && x < 100, 1, 99);
    check_inner_interval(x > 0 && x < 100, 1, 99);

    Expr c = Variable::make(Bool(), "c");
    check_outer_interval(Let::make("y", 0, x > y && x < 100), 1, 99);
    check_outer_interval(Let::make("c", x > 0, c && x < 100), 1, 99);

    check_outer_interval((x >= 10 && x <= 90) && sin(x) > 0.5f, 10, 90);
    check_inner_interval((x >= 10 && x <= 90) && sin(x) > 0.6f, pos_inf, neg_inf);

    check_inner_interval(3*x + 4 < 27, neg_inf, 7);
    check_outer_interval(3*x + 4 < 27, neg_inf, 7);

    check_inner_interval(min(x, y) > 17, 18, y);
    check_outer_interval(min(x, y) > 17, 18, pos_inf);

    debug(0) << "Solve test passed\n";

}

}
}
