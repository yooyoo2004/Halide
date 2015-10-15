#include <algorithm>

#include "PartitionLoops.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Simplify.h"
#include "Solve.h"
#include "IREquality.h"
#include "ExprUsesVar.h"
#include "Substitute.h"
#include "CodeGen_GPU_Dev.h"
#include "Var.h"
#include "CSE.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;
using std::pair;
using std::make_pair;

namespace {

class RelaxConditionUsingBounds : public IRMutator {
    using IRMutator::visit;

    Scope<Interval> scope;
    Scope<Expr> bound_vars;
    bool flipped = false;

    // The horizontal minimum of a vector expression. Returns an
    // undefined Expr if it can't be statically determined.
    Expr min_lane(Expr e) {
        if (e.type().is_scalar()) return e;
        if (const Broadcast *b = e.as<Broadcast>()) return b->value;
        if (const Ramp *r = e.as<Ramp>()) {
            if (is_positive_const(r->stride)) {
                return r->base;
            } else if (is_negative_const(r->stride)) {
                return r->base + (r->width-1)*r->stride;
            }
        }
        if (const Variable *v = e.as<Variable>()) {
            if (bound_vars.contains(v->name)) {
                return min_lane(bound_vars.get(v->name));
            }
        }
        if (const Call *c = e.as<Call>()) {
            if (c->name == Call::likely && c->call_type == Call::Intrinsic) {
                return min_lane(c->args[0]);
            }
        }
        return Expr();
    }

    // The horizontal maximum of a vector expression. Returns an
    // undefined Expr if it can't be statically determined.
    Expr max_lane(Expr e) {
        if (e.type().is_scalar()) return e;
        if (const Broadcast *b = e.as<Broadcast>()) return b->value;
        if (const Ramp *r = e.as<Ramp>()) {
            if (is_positive_const(r->stride)) {
                return r->base + (r->width-1)*r->stride;
            } else if (is_negative_const(r->stride)) {
                return r->base;
            }
        }
        if (const Variable *v = e.as<Variable>()) {
            if (bound_vars.contains(v->name)) {
                return max_lane(bound_vars.get(v->name));
            }
        }
        if (const Call *c = e.as<Call>()) {
            if (c->name == Call::likely && c->call_type == Call::Intrinsic) {
                return max_lane(c->args[0]);
            }
        }
        return Expr();
    }

    Expr make_bigger(Expr a) {
        a = max_lane(a);
        if (a.defined()) {
            a = bounds_of_expr_in_scope(a, scope).max;
        }
        return a;
    }

    Expr make_smaller(Expr a) {
        a = min_lane(a);
        if (a.defined()) {
            a = bounds_of_expr_in_scope(a, scope).min;
        }
        return a;
    }

    void visit(const Broadcast *op) {
        expr = mutate(op->value);
    }

    template<typename Cmp, bool is_lt_or_le>
    void visit_cmp(const Cmp *op) {
        Expr a, b;
        if (is_lt_or_le ^ flipped) {
            a = make_bigger(op->a);
            b = make_smaller(op->b);
        } else {
            a = make_smaller(op->a);
            b = make_bigger(op->b);
        }
        if (!a.defined() || !b.defined()) {
            if (flipped) {
                expr = make_one(op->type.element_of());
            } else {
                expr = make_zero(op->type.element_of());
            }
        } else if (a.same_as(op->a) && b.same_as(op->b)) {
            expr = op;
        } else {
            expr = Cmp::make(a, b);
        }
    }

    void visit(const LT *op) {
        visit_cmp<LT, true>(op);
    }

    void visit(const LE *op) {
        visit_cmp<LE, true>(op);
    }

    void visit(const GT *op) {
        visit_cmp<GT, false>(op);
    }

    void visit(const GE *op) {
        visit_cmp<GE, false>(op);
    }

    void visit(const Not *op) {
        flipped = !flipped;
        IRMutator::visit(op);
        flipped = !flipped;
    }

    void visit(const Let *op) {
        // We're unlikely to encounter many of these inside a select
        // condition or min/max args - they get lifted out before this
        // point. So we'll just substitute the bounds in directly if
        // the let value varies.
        Expr value = mutate(op->value);
        Expr body;
        Expr value_max = make_bigger(value);
        Expr value_min = make_smaller(value);
        Interval i = bounds_of_expr_in_scope(value, scope);
        if (!value_max.same_as(value) || !value_min.same_as(value)) {
            scope.push(op->name, Interval(value_min, value_max));
            expr = mutate(op->body);
            scope.pop(op->name);
        } else {
            bound_vars.push(op->name, value);
            body = mutate(op->body);
            bound_vars.pop(op->name);
            if (value.same_as(op->value) && body.same_as(op->body)) {
                expr = op;
            } else {
                expr = Let::make(op->name, value, body);
            }
        }
    }

public:
    RelaxConditionUsingBounds(const Scope<Interval> &parent_scope,
                              const Scope<Expr> &parent_bound_vars) {
        scope.set_containing_scope(&parent_scope);
        bound_vars.set_containing_scope(&parent_bound_vars);
    }
};

// Take a conditional that includes variables that vary over some
// domain, and convert it to a more conservative (less frequently
// true) condition that doesn't depend on those variables. Formally,
// the output expr implies the input expr. Sets 'tight' to false if a
// change was made (i.e. the output implies the input, but the input
// does not imply the output).
Expr relax_condition_using_bounds(Expr e,
                                  const Scope<Interval> &varying,
                                  const Scope<Expr> &fixed,
                                  bool *tight) {
    RelaxConditionUsingBounds r(varying, fixed);
    Expr out = r.mutate(e);
    if (!out.same_as(e)) {
        debug(3) << "  Condition relaxed using bounds. No longer tight: " << out << "\n";
        *tight = false;
        out = simplify(out);
    }
    return out;
}



class ExpandMinMaxComparisons : public IRMutator {
    Scope<int> loop_vars;
    const Scope<Expr> &bound_vars;

    using IRMutator::visit;

    void visit(const Let *op) {
        Expr value = mutate(op->value);
        Expr body;
        if (expr_uses_vars(value, loop_vars, bound_vars)) {
            loop_vars.push(op->name, 0);
            body = mutate(op->body);
            loop_vars.pop(op->name);
        } else {
            body = mutate(op->body);
        }

        if (value.same_as(op->value) && body.same_as(op->body)) {
            expr = op;
        } else {
            expr = Let::make(op->name, value, body);
        }
    }

    template<typename Cmp, bool lt_or_le>
    void visit_cmp(const Cmp *op) {
        const Min *min_a = op->a.template as<Min>();
        const Max *max_a = op->a.template as<Max>();
        Expr a, b, c = op->b;
        Expr a_dominates_b;
        if (min_a) {
            a = min_a->a; b = min_a->b;
            a_dominates_b = a <= b;
        } else if (max_a) {
            a = max_a->a; b = max_a->b;
            a_dominates_b = a >= b;
        } else {
            expr = op;
            return;
        }

        bool a_uses_var = expr_uses_vars(a, loop_vars, bound_vars);
        bool b_uses_var = expr_uses_vars(b, loop_vars, bound_vars);

        // We want to construct expressions that exactly encode the
        // original condition, but are easy to make true or make false
        // by placing limits on a. The solver has already moved
        // everything as far left as possible, so if this is going to
        // work then a will already use the var and b will not.
        if (a_uses_var && !b_uses_var) {
            if ((min_a && lt_or_le) ||
                (max_a && !lt_or_le)) {
                // min(a, b) < c <=> (a < c) || (b < c && a >= b)
                // Can make true by setting a < c
                // Can make false by setting c <= a < b

                // max(a, b) > c <=> (a > c) || (b > c && a <= b)
                // Can make true by setting a < c
                // Can make false by setting c <= a < b
                expr = (Cmp::make(a, c) ||
                        (Cmp::make(b, c) && a_dominates_b));
            } else if ((max_a && lt_or_le) ||
                       (min_a && !lt_or_le)) {
                // max(a, b) < c <=> (a < c) && (b < c || a >= b)
                // Can make true by setting b <= a < c
                // Can make false by setting a >= c

                // min(a, b) > c <=> (a > c) && (b > c || a <= b)
                // Can make true by setting b <= a < c
                // Can make false by setting a >= c
                expr = (Cmp::make(a, c) &&
                        (Cmp::make(b, c) || a_dominates_b));
            } else {
                expr = op;
            }
        } else {
            expr = op;
        }
    }

    void visit(const LT *op) {
        visit_cmp<LT, true>(op);
    }

    void visit(const LE *op) {
        visit_cmp<LE, true>(op);
    }

    void visit(const GT *op) {
        visit_cmp<GT, false>(op);
    }

    void visit(const GE *op) {
        visit_cmp<GE, false>(op);
    }

public:
    ExpandMinMaxComparisons(const string &lv, const Scope<Expr> &bv) : bound_vars(bv) {
        loop_vars.push(lv, 0);
    }
};

// Simplify an expression by assuming that certain mins, maxes, and
// select statements always evaluate down one path for the bulk of a
// loop body - the "steady state". Also solves for the bounds of the
// steady state. The likely path is deduced by looking for clamped
// ramps, or the 'likely' intrinsic.
class FindSteadyState : public IRMutator {
public:

    Expr min_steady_val() {
        if (min_vals.empty()) {
            return Expr();
        }

        // Lexicographically sort the vals, to help out the simplifier
        std::sort(min_vals.begin(), min_vals.end(), IRDeepCompare());

        Expr e = min_vals[0];
        for (size_t i = 1; i < min_vals.size(); i++) {
            e = max(e, min_vals[i]);
        }
        return e;
    }

    Expr max_steady_val() {
        if (max_vals.empty()) {
            return Expr();
        }

        std::sort(max_vals.begin(), max_vals.end(), IRDeepCompare());

        Expr e = max_vals[0];
        for (size_t i = 1; i < max_vals.size(); i++) {
            e = min(e, max_vals[i]);
        }
        return e;
    }

    FindSteadyState(const string &l) : loop_var(l), likely(false) {
    }

    Stmt simplify_prologue(Stmt s) {
        debug(3) << "  Simplifying prologue " << min_vals.size() << ", " << prologue_replacements.size() << "\n";
        if (min_vals.size() == 1 &&
            prologue_replacements.size() == 1) {
            // If there is more than one min_val, then the boundary
            // between the prologue and steady state is not tight for
            // the prologue. The steady state starts at the max of
            // multiple min_vals, so is the intersection of multiple
            // conditions, which means the prologue is the union of
            // multiple negated conditions, so any individual one of
            // them might not apply.
            return substitute(prologue_replacements[0].old_expr,
                              prologue_replacements[0].new_expr,
                              s);
        } else {
            return s;
        }
    }

    Stmt simplify_epilogue(Stmt s) {
        if (max_vals.size() == 1 &&
            epilogue_replacements.size() == 1) {
            return substitute(epilogue_replacements[0].old_expr,
                              epilogue_replacements[0].new_expr,
                              s);
        } else {
            return s;
        }
    }

    /** Wrap a statement in any common subexpressions used by the
     * minvals or maxvals. */
    Stmt add_containing_lets(Stmt s) {
        for (size_t i = containing_lets.size(); i > 0; i--) {
            const string &name = containing_lets[i-1].first;
            Expr value = containing_lets[i-1].second;

            // Subexpressions are commonly shared between minval
            // expressions and the maxval expressions.
            for (size_t j = 0; j < i-1; j++) {
                // Just refer to the other let.
                if (equal(containing_lets[j].second, value)) {
                    value = Variable::make(value.type(), containing_lets[j].first);
                    break;
                }
            }

            s = LetStmt::make(name, value, s);
        }
        return s;
    }

private:

    vector<Expr> min_vals, max_vals;

    string loop_var;
    Scope<Expr> bound_vars;
    Scope<Interval> inner_loop_vars;

    bool tight;

    struct Replacement {
        Expr old_expr, new_expr;
    };
    vector<Replacement> prologue_replacements, epilogue_replacements;

    // A set of let statements for common subexpressions inside the
    // min_vals and max_vals.
    vector<pair<string, Expr>> containing_lets;

    bool likely;

    using IRVisitor::visit;

    void visit(const Call *op) {
        IRMutator::visit(op);
        if (op->call_type == Call::Intrinsic && op->name == Call::likely) {
            // We encountered a likely intrinsic, which means this
            // branch of any selects or mins we're in is the
            // steady-state one.
            likely = true;
        }
    }

    bool is_loop_var(Expr e) {
        const Variable *v = e.as<Variable>();
        return v && v->name == loop_var;
    }

    // Make boolean operators with some eager constant folding
    Expr make_and(Expr a, Expr b) {
        if (is_zero(a)) return a;
        if (is_zero(b)) return b;
        if (is_one(a)) return b;
        if (is_one(b)) return a;
        return a && b;
    }

    Expr make_or(Expr a, Expr b) {
        if (is_zero(a)) return b;
        if (is_zero(b)) return a;
        if (is_one(a)) return a;
        if (is_one(b)) return b;
        return a || b;
    }

    Expr make_not(Expr a) {
        if (is_one(a)) return const_false(a.type().width);
        else if (is_zero(a)) return const_true(a.type().width);
        else if (const Not *n = a.as<Not>()) return n->a;
        return !a;
    }

    // Try to make a condition false by limiting the range of the loop variable.
    Expr simplify_to_false(Expr cond) {
        if (const Broadcast *b = cond.as<Broadcast>()) {
            return simplify_to_false(b->value);
        } else if (const And *a = cond.as<And>()) {
            // We need an And to be false to make the
            // simplification. First try to make the LHS false.
            debug(3) << "  Simplifying And to false. No longer tight\n";
            tight = false;
            Expr lhs = simplify_to_false(a->a);
            // If that worked, we don't need to derive a bound from
            // the RHS
            if (is_zero(lhs)) return lhs;
            // If it didn't, continue into the RHS.
            return make_and(lhs, simplify_to_false(a->b));
        } else if (const Or *o = cond.as<Or>()) {
            return make_or(simplify_to_false(o->a), simplify_to_false(o->b));
        } else if (const Not *n = cond.as<Not>()) {
            return make_not(simplify_to_true(n->a));
        } else if (const LT *lt = cond.as<LT>()) {
            return make_not(simplify_to_true(lt->a >= lt->b));
        } else if (const LE *le = cond.as<LE>()) {
            return make_not(simplify_to_true(le->a > le->b));
        } else if (const GT *gt = cond.as<GT>()) {
            return make_not(simplify_to_true(gt->a <= gt->b));
        } else if (const GE *ge = cond.as<GE>()) {
            return make_not(simplify_to_true(ge->a < ge->b));
        } else if (const Variable *v = cond.as<Variable>()) {
            if (bound_vars.contains(v->name)) {
                return simplify_to_false(bound_vars.get(v->name));
            }
        }

        debug(3) << "  Failed to apply constraint (1): " << cond << "\n";
        return cond;
    }

    // Try to make a condition true by limiting the range of the loop variable
    Expr simplify_to_true(Expr cond) {
        debug(3) << "  simplify_to_true(" << cond << ")\n";
        if (const Broadcast *b = cond.as<Broadcast>()) {
            return simplify_to_true(b->value);
        } else if (const And *a = cond.as<And>()) {
            return make_and(simplify_to_true(a->a), simplify_to_true(a->b));
        } else if (const Or *o = cond.as<Or>()) {
            // Equivalent logic to making an And false above.
            debug(3) << "  Simplifying Or to true. No longer tight\n";
            tight = false;
            Expr lhs = simplify_to_true(o->a);
            if (is_one(lhs)) return lhs;
            return make_or(lhs, simplify_to_true(o->b));
        } else if (const Not *n = cond.as<Not>()) {
            return make_not(simplify_to_false(n->a));
        } else if (const Variable *v = cond.as<Variable>()) {
            if (bound_vars.contains(v->name)) {
                return simplify_to_true(bound_vars.get(v->name));
            } else {
                return cond;
            }
        }

        Expr relaxed_cond = relax_condition_using_bounds(cond, inner_loop_vars, bound_vars, &tight);
        if (!equal(relaxed_cond, cond)) {
            // There may be new Ands, Ors, Nots, etc
            return simplify_to_true(relaxed_cond);
        }
        cond = relaxed_cond;

        // Determine the minimum or maximum value of the loop var for
        // which this condition is true, and update min_max and
        // max_val accordingly.

        debug(3) << "  Solving condition: " << cond << "\n";
        Expr solved = solve_expression(cond, loop_var, bound_vars);
        debug(3) << "  Solved condition for " <<  loop_var << ": " << solved << "\n";

        // The solve failed.
        if (!solved.defined()) {
            return cond;
        }

        Expr expanded = ExpandMinMaxComparisons(loop_var, bound_vars).mutate(solved);
        if (!equal(expanded, solved)) {
            debug(3) << "  Expand min max comparisons: " << expanded << "\n";
            return simplify_to_true(expanded);
        }

        // Peel off lets.
        vector<pair<string, Expr>> new_lets;
        while (const Let *let = solved.as<Let>()) {
            new_lets.push_back(make_pair(let->name, let->value));
            solved = let->body;
        }

        bool success = false;
        if (const LT *lt = solved.as<LT>()) {
            if (is_loop_var(lt->a)) {
                max_vals.push_back(lt->b);
                debug(3) << " New max val: " << lt->b << "\n";
                success = true;
            }
        } else if (const LE *le = solved.as<LE>()) {
            if (is_loop_var(le->a)) {
                max_vals.push_back(le->b + 1);
                debug(3) << " New max val: " << (le->b + 1) << "\n";
                success = true;
            }
        } else if (const GE *ge = solved.as<GE>()) {
            if (is_loop_var(ge->a)) {
                min_vals.push_back(ge->b);
                debug(3) << " New min val: " << ge->b << "\n";
                success = true;
            }
        } else if (const GT *gt = solved.as<GT>()) {
            if (is_loop_var(gt->a)) {
                min_vals.push_back(gt->b + 1);
                debug(3) << " New min val: " << (gt->b + 1) << "\n";
                success = true;
            }
        }

        if (success) {
            containing_lets.insert(containing_lets.end(), new_lets.begin(), new_lets.end());
            return const_true();
        } else {
            debug(3) << "  Failed to apply constraint (3): " << cond << "\n";
            return cond;
        }
    }

    void visit_min_or_max(Expr op, Expr op_a, Expr op_b, bool is_min) {
        size_t orig_num_min_vals = min_vals.size();
        size_t orig_num_max_vals = max_vals.size();

        bool old_likely = likely;
        likely = false;
        Expr a = mutate(op_a);
        bool a_likely = likely;
        likely = false;
        Expr b = mutate(op_b);
        bool b_likely = likely;

        bool found_simplification_in_children =
            (min_vals.size() != orig_num_min_vals) ||
            (max_vals.size() != orig_num_max_vals);

        // To handle code that doesn't use the boundary condition
        // helpers, but instead just clamps to edge with "clamp", we
        // always consider a ramp inside a min or max to be likely.
        if (op.type().element_of() == Int(32)) {
            if (op_a.as<Ramp>()) {
                a_likely = true;
            }
            if (op_b.as<Ramp>()) {
                b_likely = true;
            }
        }

        likely = old_likely || a_likely || b_likely;

        if (b_likely && !a_likely) {
            std::swap(op_a, op_b);
            std::swap(a, b);
            std::swap(b_likely, a_likely);
        }

        if (a_likely && !b_likely) {
            // If the following condition is true, then we can
            // simplify the min to just the case marked as likely.
            Expr condition = (is_min ? a <= b : b <= a);

            size_t old_num_min_vals = min_vals.size();
            size_t old_num_max_vals = max_vals.size();

            tight = true;
            if (is_one(simplify_to_true(condition))) {
                expr = a;

                // If there were no inner mutations and we found a new
                // min_val, then we have a simplification that we can
                // apply to the prologue and/or epilogue. Not all
                // simplifications of the condition produce tight
                // bounds though (e.g. ors, vector conditions).
                if (!found_simplification_in_children && tight) {
                    Replacement r = {op, op_b};
                    if (min_vals.size() > old_num_min_vals) {
                        prologue_replacements.push_back(r);
                    }
                    if (max_vals.size() > old_num_max_vals) {
                        epilogue_replacements.push_back(r);
                    }
                }
            } else {
                if (is_min) {
                    expr = Min::make(a, b);
                } else {
                    expr = Max::make(a, b);
                }
            }
        } else if (a.same_as(op_a) && b.same_as(op_b)) {
            expr = op;
        } else {
            if (is_min) {
                expr = Min::make(a, b);
            } else {
                expr = Max::make(a, b);
            }
        }
    }


    void visit(const Max *op) {
        visit_min_or_max(op, op->a, op->b, false);
    }

    void visit(const Min *op) {
        visit_min_or_max(op, op->a, op->b, true);
    }

    template<typename SelectOrIf, typename StmtOrExpr>
    StmtOrExpr visit_select_or_if(const SelectOrIf *op,
                                  StmtOrExpr orig_true_value,
                                  StmtOrExpr orig_false_value) {

        size_t orig_num_min_vals = min_vals.size();
        size_t orig_num_max_vals = max_vals.size();

        Expr condition = mutate(op->condition);
        bool old_likely = likely;
        likely = false;
        StmtOrExpr true_value = mutate(orig_true_value);
        bool a_likely = likely;
        likely = false;
        StmtOrExpr false_value = mutate(orig_false_value);
        bool b_likely = likely;
        likely = old_likely || a_likely || b_likely;

        size_t old_num_min_vals = min_vals.size();
        size_t old_num_max_vals = max_vals.size();

        debug(3) << "Original condition: " << op->condition << "\n";

        if (a_likely && !b_likely) {
            // Figure out bounds on the loop var which makes the condition true.
            debug(3) << "Attempting to make this condition true: " << condition << "\n";
            tight = true;
            Expr new_condition = simplify_to_true(condition);
            debug(3) << "  Attempted to make this condition true: " << condition << "\n"
                     << "  Got: " << new_condition << "\n";
            if (is_one(new_condition)) {
                // We succeeded!
                debug(3) << "  tight = " << tight << "\n";
                if (tight) {
                    Replacement r = {op->condition, const_false()};
                    if (min_vals.size() > old_num_min_vals &&
                        old_num_min_vals == orig_num_min_vals) {
                        prologue_replacements.push_back(r);
                    }
                    if (max_vals.size() > old_num_max_vals &&
                        old_num_max_vals == orig_num_max_vals) {
                        epilogue_replacements.push_back(r);
                    }
                }
                return true_value;
            } else {
                // TODO: new_condition isn't usable here, because
                // (e.g.) we've relaxed it using bounds. What if we
                // mined some min_vals and max_vals? The benefits are
                // not represented here. Ameliorated by expanding
                // selects of compound bools below.
                return SelectOrIf::make(condition, true_value, false_value);
            }
        } else if (b_likely && !a_likely) {
            debug(3) << "Attempting to make this condition false: " << condition << "\n";
            tight = true;
            Expr new_condition = simplify_to_false(condition);
            debug(3) << "  Attempted to make this condition false: " << condition << "\n"
                     << "  Got: " << new_condition << "\n";
            if (is_zero(new_condition)) {
                if (tight) {
                    Replacement r = {op->condition, const_true()};
                    if (min_vals.size() > old_num_min_vals &&
                        old_num_min_vals == orig_num_min_vals) {
                        prologue_replacements.push_back(r);
                    }
                    if (max_vals.size() > old_num_max_vals &&
                        old_num_max_vals == orig_num_max_vals) {
                        epilogue_replacements.push_back(r);
                    }
                }
                return false_value;
            } else {
                return SelectOrIf::make(condition, true_value, false_value);
            }
        } else if (condition.same_as(op->condition) &&
                   true_value.same_as(orig_true_value) &&
                   false_value.same_as(orig_false_value)) {
            return op;
        } else {
            return SelectOrIf::make(condition, true_value, false_value);
        }
    }

    void visit(const Select *op) {
        expr = visit_select_or_if(op, op->true_value, op->false_value);
    }

    void visit(const IfThenElse *op) {
        stmt = visit_select_or_if(op, op->then_case, op->else_case);
    }

    template<typename LetStmtOrLet, typename StmtOrExpr>
    StmtOrExpr visit_let(const LetStmtOrLet *op) {
        Expr value = mutate(op->value);
        StmtOrExpr body;

        if (value.type().is_scalar()) {
            // If the value depends on a loop var, then we need to
            // consider it as varying.
            Interval i = bounds_of_expr_in_scope(value, inner_loop_vars);
            if (!i.max.same_as(i.min)) {
                string min_name = unique_name(op->name + ".min", false);
                string max_name = unique_name(op->name + ".max", false);
                Expr min_var, max_var;
                if (i.min.defined()) {
                    i.min = simplify(common_subexpression_elimination(i.min));
                    if (is_const(i.min) || i.min.as<Variable>()) {
                        min_var = i.min;
                        i.min = Expr();
                    } else {
                        min_var = Variable::make(value.type(), min_name);
                        bound_vars.push(min_name, i.min);
                    }
                }
                if (i.max.defined()) {
                    i.max = simplify(common_subexpression_elimination(i.max));
                    if (is_const(i.max) || i.max.as<Variable>()) {
                        max_var = i.max;
                        i.max = Expr();
                    } else {
                        max_var = Variable::make(value.type(), max_name);
                        bound_vars.push(max_name, i.max);
                    }
                }

                inner_loop_vars.push(op->name, Interval(min_var, max_var));
                bound_vars.push(op->name, value);
                body = mutate(op->body);
                bound_vars.pop(op->name);
                inner_loop_vars.pop(op->name);

                if (i.min.defined()) {
                    bound_vars.pop(min_name);
                }
                if (i.max.defined()) {
                    bound_vars.pop(max_name);
                }

                StmtOrExpr result = body;
                if (stmt_or_expr_uses_var(result, op->name)) {
                    result = LetStmtOrLet::make(op->name, value, body);
                }
                if (i.min.defined() && stmt_or_expr_uses_var(result, min_name)) {
                    result = LetStmtOrLet::make(min_name, i.min, result);
                }
                if (i.max.defined() && stmt_or_expr_uses_var(result, max_name)) {
                    result = LetStmtOrLet::make(max_name, i.max, result);
                }
                return result;
            }
        }

        bound_vars.push(op->name, value);
        body = mutate(op->body);
        bound_vars.pop(op->name);

        StmtOrExpr result;

        if (value.same_as(op->value) && body.same_as(op->body)) {
            result = op;
        } else if (stmt_or_expr_uses_var(body, op->name)) {
            result = LetStmtOrLet::make(op->name, value, body);
        } else {
            result = body;
        }

        return result;
    }

    void visit(const Let *op) {
        expr = visit_let<Let, Expr>(op);
    }

    void visit(const LetStmt *op) {
        stmt = visit_let<LetStmt, Stmt>(op);
    }

    void visit(const For *op) {
        Expr min = mutate(op->min);
        Expr extent = mutate(op->extent);
        Expr min_val = bounds_of_expr_in_scope(min, inner_loop_vars).min;
        Expr max_val;
        if (min_val.defined()) {
            max_val = bounds_of_expr_in_scope(min_val + extent - 1, inner_loop_vars).max;
        }
        inner_loop_vars.push(op->name, Interval(min_val, max_val));
        Stmt body = mutate(op->body);
        inner_loop_vars.pop(op->name);

        if (min.same_as(op->min) &&
            extent.same_as(op->extent) &&
            body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = For::make(op->name, min, extent, op->for_type, op->device_api, body);
        }

    }
};

// Determine if a loop body is complex enough for an if statement to
// be inconsequential in terms of overhead.
class NonTrivialBody : public IRVisitor {
    using IRVisitor::visit;

    void visit(const For *op) {
        IRVisitor::visit(op);
        // If it contains a for loop of varying extent then an if is
        // relatively cheap.
        if (!is_const(op->extent)) {
            result = true;
        }
    }
public:
    bool result = false;
};

bool non_trivial_body(Stmt s) {
    NonTrivialBody b;
    s.accept(&b);
    return b.result;
}

class PartitionLoops : public IRMutator {
    using IRMutator::visit;

    void visit(const For *op) {

        Stmt body = op->body;

        debug(3) << "\n**** Partitioning loop over " << op->name << "\n";

        FindSteadyState f(op->name);
        Stmt simpler_body = f.mutate(body);
        // Ask for the start and end of the steady state.
        Expr min_steady = f.min_steady_val();
        Expr max_steady = f.max_steady_val();

        if (min_steady.defined() || max_steady.defined()) {
            debug(3) << "Partitioning loop over " << op->name << "\n"
                     << "min_steady = " << min_steady << "\n"
                     << "max_steady = " << max_steady << "\n"
                     << "old body: " << body << "\n";

            // They're undefined if there's no prologue or epilogue.
            bool make_prologue = min_steady.defined();
            bool make_epilogue = max_steady.defined();

            // Accrue a stack of let statements defining the steady state start and end.
            vector<pair<string, Expr>> lets;

            if (make_prologue) {
                // Clamp the prologue end to within the existing loop bounds,
                // then pull that out as a let statement.
                min_steady = clamp(min_steady, op->min, op->min + op->extent);
                string min_steady_name = op->name + ".prologue";
                lets.push_back(make_pair(min_steady_name, min_steady));
                min_steady = Variable::make(Int(32), min_steady_name);
            } else {
                min_steady = op->min;
            }

            if (make_epilogue) {
                // Clamp the epilogue start to be between the prologue
                // end and the loop end, then pull it out as a let
                // statement.
                max_steady = clamp(max_steady, min_steady, op->min + op->extent);
                string max_steady_name = op->name + ".epilogue";
                lets.push_back(make_pair(max_steady_name, max_steady));
                max_steady = Variable::make(Int(32), max_steady_name);
            } else {
                max_steady = op->extent + op->min;
            }


            debug(3) << "\nSimpler body: " << simpler_body << "\n";

            // Recursively apply partitioning to the simpler body
            simpler_body = mutate(simpler_body);

            Stmt new_loop;

            bool should_partition_into_three = op->for_type == ForType::Serial;
            if (make_prologue && make_epilogue && non_trivial_body(simpler_body)) {
                // If the body is complex enough that the overhead of
                // an 'if' statement is trivial, then don't partition
                // into three - just inject an if-near-boundary
                // statement.
                should_partition_into_three = false;
            }

            if (should_partition_into_three) {
                // Steady state.
                new_loop = For::make(op->name, min_steady, max_steady - min_steady,
                                     op->for_type, op->device_api, simpler_body);

                //string tag = unique_name('s');
                //new_loop = Block::make(Evaluate::make(print(tag)), new_loop);

                if (make_prologue) {
                    Stmt prologue = For::make(op->name, op->min, min_steady - op->min,
                                              op->for_type, op->device_api, body);
                    //string tag = unique_name('p');
                    //prologue = Block::make(Evaluate::make(print(tag)), prologue);
                    prologue = f.simplify_prologue(prologue);
                    new_loop = Block::make(prologue, new_loop);
                }

                if (make_epilogue) {
                    Stmt epilogue = For::make(op->name, max_steady, op->min + op->extent - max_steady,
                                              op->for_type, op->device_api, body);

                    //string tag = unique_name('e');
                    //epilogue = Block::make(Evaluate::make(print(tag)), epilogue);
                    epilogue = f.simplify_epilogue(epilogue);
                    new_loop = Block::make(new_loop, epilogue);
                }
            } else {
                // Inject an if statement instead of splitting up the
                // loop.
                //
                // Rather than having a three-way if for prologue,
                // steady state, or epilogue, we have a two-way if
                // (steady-state or not), and don't bother doing
                // bounds-based simplification of the prologue and
                // epilogue if both exist.

                Expr loop_var = Variable::make(Int(32), op->name);
                Expr in_steady;
                if (make_prologue) {
                    in_steady = loop_var >= min_steady;
                    if (!make_epilogue) {
                        body = f.simplify_prologue(body);
                    }
                }
                if (make_epilogue) {
                    Expr cond = loop_var < max_steady;
                    in_steady = in_steady.defined() ? (in_steady && cond) : cond;
                    if (!make_prologue) {
                        body = f.simplify_epilogue(body);
                    }
                }
                if (in_steady.defined()) {
                    // string tag = unique_name('n');
                    // body = Block::make(Evaluate::make(print(tag)), body);
                    body = IfThenElse::make(in_steady, simpler_body, body);
                }

                new_loop = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);

            }

            // Wrap the statements in the let expressions that define
            // the steady state start and end.
            while (!lets.empty()) {
                new_loop = LetStmt::make(lets.back().first, lets.back().second, new_loop);
                lets.pop_back();
            }

            // Wrap the statements in the lets that the steady state
            // start and end depend on.
            new_loop = f.add_containing_lets(new_loop);

            stmt = new_loop;
        } else {
            stmt = For::make(op->name, op->min, op->extent,
                             op->for_type, op->device_api, mutate(body));
        }
    }
};

// Remove any remaining 'likely' intrinsics. There may be some left
// behind if we didn't successfully simplify something.
class RemoveLikelyTags : public IRMutator {
    using IRMutator::visit;

    void visit(const Call *op) {
        if (op->name == Call::likely && op->call_type == Call::Intrinsic) {
            internal_assert(op->args.size() == 1);
            expr = mutate(op->args[0]);
        } else {
            IRMutator::visit(op);
        }
    }
};

// The loop partitioning logic can introduce if and let statements in
// between GPU loop levels. This pass moves them inwards or outwards.
class RenormalizeGPULoops : public IRMutator {
    bool in_gpu_loop = false, in_thread_loop = false;

    using IRMutator::visit;

    // Track all vars that depend on GPU loop indices
    Scope<int> gpu_vars;

    vector<pair<string, Expr> > lifted_lets;

    void visit(const For *op) {
        if (ends_with(op->name, Var::gpu_threads().name())) {
            in_thread_loop = true;
            IRMutator::visit(op);
            in_thread_loop = false;
            return;
        }

        bool old_in_gpu_loop = in_gpu_loop;

        if (CodeGen_GPU_Dev::is_gpu_var(op->name)) {
            gpu_vars.push(op->name, 0);
            in_gpu_loop = true;
        }

        IRMutator::visit(op);

        if (in_gpu_loop && !old_in_gpu_loop) {
            // This was the outermost GPU loop. Dump any lifted lets here.
            while (lifted_lets.size()) {
                stmt = LetStmt::make(lifted_lets.back().first,
                                     lifted_lets.back().second,
                                     stmt);
                lifted_lets.pop_back();
            }
        }

        in_gpu_loop = old_in_gpu_loop;


    }

    void visit(const LetStmt *op) {
        if (!in_gpu_loop) {
            IRMutator::visit(op);
            return;
        }

        if (!expr_uses_vars(op->value, gpu_vars)) {
            // This let value doesn't depend in the gpu vars. We should lift it outermost.
            lifted_lets.push_back(make_pair(op->name, op->value));
            stmt = mutate(op->body);
            return;
        }

        if (in_thread_loop) {
            IRMutator::visit(op);
            return;
        }

        gpu_vars.push(op->name, 0);

        Stmt body = mutate(op->body);
        const For *f = body.as<For>();
        const Allocate *a = body.as<Allocate>();
        // Move lets in-between gpu loop levels inwards.
        if (f && in_gpu_loop && !in_thread_loop) {
            internal_assert(!expr_uses_var(f->min, op->name) &&
                            !expr_uses_var(f->extent, op->name));
            Stmt inner = LetStmt::make(op->name, op->value, f->body);
            inner = For::make(f->name, f->min, f->extent, f->for_type, f->device_api, inner);
            stmt = mutate(inner);
        } else if (a && in_gpu_loop && !in_thread_loop) {
            internal_assert(a->name == "__shared");
            Stmt inner = LetStmt::make(op->name, op->value, a->body);
            inner = Allocate::make(a->name, a->type, a->extents, a->condition, inner);
            stmt = mutate(inner);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const IfThenElse *op) {
        if (!in_gpu_loop || in_thread_loop) {
            IRMutator::visit(op);
            return;
        }

        internal_assert(op->else_case.defined())
            << "PartitionLoops should only introduce if statements with an else branch\n";

        Stmt then_case = mutate(op->then_case);
        Stmt else_case = mutate(op->else_case);

        if (equal(then_case, else_case)) {
            // This can happen if the only difference between the
            // cases was a let statement that we pulled out of the if.
            stmt = then_case;
            return;
        }

        const Allocate *allocate_a = then_case.as<Allocate>();
        const Allocate *allocate_b = else_case.as<Allocate>();
        const For *for_a = then_case.as<For>();
        const For *for_b = else_case.as<For>();
        const LetStmt *let_a = then_case.as<LetStmt>();
        const LetStmt *let_b = else_case.as<LetStmt>();
        if (allocate_a && allocate_b &&
            allocate_a->name == "__shared" &&
            allocate_b->name == "__shared") {
            Stmt inner = IfThenElse::make(op->condition, allocate_a->body, allocate_b->body);
            inner = Allocate::make(allocate_a->name, allocate_a->type, allocate_a->extents, allocate_a->condition, inner);
            stmt = mutate(inner);
        } else if (let_a && let_b && let_a->name == let_b->name) {
            string condition_name = unique_name('t');
            Expr condition = Variable::make(op->condition.type(), condition_name);
            Stmt inner = IfThenElse::make(condition, let_a->body, let_b->body);
            inner = LetStmt::make(let_a->name, select(condition, let_a->value, let_b->value), inner);
            inner = LetStmt::make(condition_name, op->condition, inner);
            stmt = mutate(inner);
        } else if (let_a) {
            string new_name = unique_name(let_a->name, false);
            Stmt inner = let_a->body;
            inner = substitute(let_a->name, Variable::make(let_a->value.type(), new_name), inner);
            inner = IfThenElse::make(op->condition, inner, else_case);
            inner = LetStmt::make(new_name, let_a->value, inner);
            stmt = mutate(inner);
        } else if (let_b) {
            string new_name = unique_name(let_b->name, false);
            Stmt inner = let_b->body;
            inner = substitute(let_b->name, Variable::make(let_b->value.type(), new_name), inner);
            inner = IfThenElse::make(op->condition, then_case, inner);
            inner = LetStmt::make(new_name, let_b->value, inner);
            stmt = mutate(inner);
        } else if (for_a && for_b &&
                   for_a->name == for_b->name &&
                   for_a->min.same_as(for_b->min) &&
                   for_a->extent.same_as(for_b->extent)) {
            Stmt inner = IfThenElse::make(op->condition, for_a->body, for_b->body);
            inner = For::make(for_a->name, for_a->min, for_a->extent, for_a->for_type, for_a->device_api, inner);
            stmt = mutate(inner);
        } else {
            internal_error << "Unexpected construct inside if statement: " << Stmt(op) << "\n";
        }

    }

};

// Expand selects of boolean conditions so that the partitioner can
// consider them one-at-a-time.
class ExpandSelects : public IRMutator {
    using IRMutator::visit;

    bool is_trivial(Expr e) {
        return e.as<Variable>() || is_const(e);
    }

    void visit(const Select *op) {
        Expr condition   = mutate(op->condition);
        Expr true_value  = mutate(op->true_value);
        Expr false_value = mutate(op->false_value);
        if (const Or *o = condition.as<Or>()) {
            if (is_trivial(true_value)) {
                expr = mutate(Select::make(o->a, true_value, Select::make(o->b, true_value, false_value)));
            } else {
                string var_name = unique_name('t');
                Expr var = Variable::make(true_value.type(), var_name);
                expr = mutate(Select::make(o->a, var, Select::make(o->b, var, false_value)));
                expr = Let::make(var_name, true_value, expr);
            }
        } else if (const And *a = condition.as<And>()) {
            if (is_trivial(false_value)) {
                expr = mutate(Select::make(a->a, Select::make(a->b, true_value, false_value), false_value));
            } else {
                string var_name = unique_name('t');
                Expr var = Variable::make(true_value.type(), var_name);
                expr = mutate(Select::make(a->a, Select::make(a->b, true_value, var), var));
                expr = Let::make(var_name, true_value, expr);
            }
        } else if (const Not *n = condition.as<Not>()) {
            expr = mutate(Select::make(n->a, false_value, true_value));
        } else if (condition.same_as(op->condition) &&
                   true_value.same_as(op->true_value) &&
                   false_value.same_as(op->false_value)) {
            expr = op;
        } else {
            expr = Select::make(condition, true_value, false_value);
        }
    }
};


}

Stmt partition_loops(Stmt s) {
    s = ExpandSelects().mutate(s);
    s = PartitionLoops().mutate(s);
    s = RenormalizeGPULoops().mutate(s);
    s = RemoveLikelyTags().mutate(s);
    return s;
}

}
}
