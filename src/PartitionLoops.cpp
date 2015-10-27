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
using std::map;

namespace {

// Loop partitioning only applies to things marked as 'likely'. Loads
// through hand-written boundary conditions will produce clamped
// ramps, which will turn into gathers. This pass injects likely
// intrinsics so that these clamped ramps are picked up by loop
// partitioning.
class MarkClampedRampsAsLikely : public IRMutator {
    using IRMutator::visit;
    void visit(const Min *op) {
        if (in_index && op->a.as<Ramp>()) {
            // No point recursing into the ramp - it can't contain
            // another ramp.
            expr = min(likely(op->a), mutate(op->b));
        } else if (in_index && op->b.as<Ramp>()) {
            expr = min(mutate(op->a), likely(op->b));
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Max *op) {
        if (in_index && op->a.as<Ramp>()) {
            expr = max(likely(op->a), mutate(op->b));
        } else if (in_index && op->b.as<Ramp>()) {
            expr = max(mutate(op->a), likely(op->b));
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Load *op) {
        bool old_in_index = in_index;
        in_index = true;
        IRMutator::visit(op);
        in_index = old_in_index;
    }

    void visit(const Store *op) {
        bool old_in_index = in_index;
        in_index = true;
        Expr index = mutate(op->index);
        in_index = old_in_index;
        Expr value = mutate(op->value);
        if (index.same_as(op->index) && value.same_as(op->value)) {
            stmt = op;
        } else {
            stmt = Store::make(op->name, value, index);
        }
    }

    bool in_index = false;
};

// Take a vector expression and convert it into an expression giving
// the value of the i'th lane, where is a new variable of the
// requested name.
class ConvertVectorLaneToFreeVar : public IRMutator {
    Expr lane_var;

    using IRMutator::visit;

    void visit(const Ramp *op) {
        expr = op->base + cast(op->base.type(), lane_var) * op->stride;
    }

    void visit(const Broadcast *op) {
        expr = op->value;
    }

    void visit(const Call *op) {
        if (op->type.is_vector()) {
            internal_assert(op->name != Call::shuffle_vector &&
                            op->name != Call::interleave_vectors);
            vector<Expr> args;
            for (Expr a : op->args) {
                args.push_back(mutate(a));
            }
            expr = Call::make(op->type.element_of(), op->name, args, op->call_type);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Load *op) {
        if (op->type.is_vector()) {
            expr = Load::make(op->type.element_of(), op->name, mutate(op->index), op->image, op->param);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Cast *op) {
        if (op->type.is_vector()) {
            expr = Cast::make(op->type.element_of(), mutate(op->value));
        } else {
            IRMutator::visit(op);
        }
    }

    Scope<Expr> inner_lets;

    void visit(const Let *op) {
        Expr value = mutate(op->value);
        inner_lets.push(op->name, value);
        Expr body = mutate(op->body);
        inner_lets.pop(op->name);

        if (value.same_as(op->value) && body.same_as(op->body)) {
            expr = op;
        } else {
            expr = Let::make(op->name, value, body);
        }
    }

    void visit(const Variable *op) {
        if (op->type.is_vector() && inner_lets.contains(op->name)) {
            expr = Variable::make(op->type.element_of(), op->name);
        } else if (op->type.is_vector()) {
            // Uh oh
            internal_error << "TODO";
        } else {
            expr = op;
        }
    }

public:
    ConvertVectorLaneToFreeVar(const string &v) {
        lane_var = Variable::make(Int(32), v);
    }

};

class AndConditionOverDomain : public IRMutator {
    using IRMutator::visit;

    Scope<Interval> scope;
    Scope<Expr> bound_vars;
    bool flipped = false;

    Interval get_bounds(Expr a) {
        Interval bounds;
        if (a.type().is_vector()) {
            string v = unique_name('v');
            scope.push(v, Interval(0, a.type().width-1));
            a = ConvertVectorLaneToFreeVar(v).mutate(a);
            bounds = bounds_of_expr_in_scope(a, scope);
            scope.pop(v);
        } else {
            bounds = bounds_of_expr_in_scope(a, scope);
        }
        if (!bounds.min.same_as(bounds.max) ||
            !bounds.min.defined() ||
            !bounds.max.defined()) {
            relaxed = true;
        }
        return bounds;
    }

    Expr make_bigger(Expr a) {
        return get_bounds(a).max;
    }

    Expr make_smaller(Expr a) {
        return get_bounds(a).min;
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

    void visit(const EQ *op) {
        if (op->type.is_vector()) {
            if (flipped) {
                expr = make_one(op->type.element_of());
            } else {
                expr = make_zero(op->type.element_of());
            }
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const NE *op) {
        expr = mutate(!(op->a == op->b));
    }

    void visit(const Not *op) {
        flipped = !flipped;
        IRMutator::visit(op);
        flipped = !flipped;
    }

    void visit(const Variable *op) {
        if (scope.contains(op->name) && op->type.is_bool()) {
            Interval i = scope.get(op->name);
            if (!flipped) {
                if (i.min.defined()) {
                    // Be conservative
                    expr = i.min;
                } else {
                    expr = const_false();
                }
            } else {
                if (i.max.defined()) {
                    expr = i.max;
                } else {
                    expr = const_true();
                }
            }
        } else {
            expr = op;
        }

    }

    void visit(const Let *op) {
        // If it's a numeric value, we can just get the bounds of
        // it. If it's a boolean value yet, we don't know whether it
        // would be more conservative to make it true or to make it
        // false, because we don't know how it will be used. We'd
        // better take the union over both options.
        Expr value = mutate(op->value);
        Expr body;
        Expr max_value = make_bigger(value);
        Expr min_value = make_smaller(value);

        if (op->value.type().is_bool()) {
            flipped = !flipped;
            Expr flipped_value = mutate(op->value);
            if (!equal(value, flipped_value)) {
                min_value = const_false();
                max_value = const_true();
            }
            flipped = !flipped;
        }

        if (!max_value.same_as(value) || !min_value.same_as(value)) {
            string min_name = unique_name(op->name + ".min", false);
            string max_name = unique_name(op->name + ".max", false);
            Expr min_var, max_var;
            if (!min_value.defined() ||
                (is_const(min_value) && min_value.as<Variable>())) {
                min_var = min_value;
                min_value = Expr();
            } else {
                min_var = Variable::make(min_value.type(), min_name);
            }
            if (!max_value.defined() ||
                (is_const(max_value) && max_value.as<Variable>())) {
                max_var = max_value;
                max_value = Expr();
            } else {
                max_var = Variable::make(max_value.type(), max_name);
            }

            scope.push(op->name, Interval(min_var, max_var));
            expr = mutate(op->body);
            scope.pop(op->name);

            if (expr_uses_var(expr, op->name)) {
                if (op->value.type().is_bool()) {
                    internal_error << "Should have removed inner boolean variable\n";
                } else {
                    expr = Let::make(op->name, value, expr);
                }
            }
            if (min_value.defined() && expr_uses_var(expr, min_name)) {
                expr = Let::make(min_name, min_value, expr);
            }
            if (max_value.defined() && expr_uses_var(expr, max_name)) {
                expr = Let::make(max_name, max_value, expr);
            }
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
    bool relaxed = false;

    AndConditionOverDomain(const Scope<Interval> &parent_scope) {
        scope.set_containing_scope(&parent_scope);
    }
};

// Take a conditional that includes variables that vary over some
// domain, and convert it to a more conservative (less frequently
// true) condition that doesn't depend on those variables. Formally,
// the output expr implies the input expr. Sets 'tight' to false if a
// change was made (i.e. the output implies the input, but the input
// does not imply the output).
//
// The condition may be a vector condition, in which case we also
// 'and' over the vector lanes, and return a scalar result.
Expr and_condition_over_domain(Expr e,
                               const Scope<Interval> &varying,
                               bool *tight) {
    AndConditionOverDomain r(varying);
    Expr out = r.mutate(e);
    if (r.relaxed) {
        debug(3) << "  Condition made more conservative using bounds. No longer tight:\n"
                 << "    " << e << "\n"
                 << "    " << out << "\n";
        *tight = false;
        out = simplify(out);
    }
    return out;
}

// Remove any 'likely' intrinsics.
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

class HasLikelyTag : public IRVisitor {
    using IRVisitor::visit;
    void visit(const Call *op) {
        if (op->name == Call::likely &&
            op->call_type == Call::Intrinsic) {
            result = true;
        } else {
            IRVisitor::visit(op);
        }
    }
public:
    bool result = false;
};

bool has_likely_tag(Expr e) {
    HasLikelyTag h;
    e.accept(&h);
    return h.result;
}

struct Simplification {
    // This condition is sufficient for the simplification to occur.
    Expr condition;
    // The expression we're simplifying
    Expr old_expr;
    // The replacement if the condition is true
    Expr likely_value;
    // The replacement if the condition is false. Not useful
    // unless it's tight.
    Expr unlikely_value;
    // Is the condition necessary (as well as sufficient)?
    bool tight;
    // The interval over which this simplification applies. Comes from solving the condition.
    Interval interval;
};

class FindSimplifications : public IRVisitor {
    using IRVisitor::visit;

public:
    vector<Simplification> simplifications;

private:
    void new_simplification(Expr condition, Expr old, Expr likely_val, Expr unlikely_val) {
        condition = RemoveLikelyTags().mutate(condition);
        Simplification s = {condition, old, likely_val, unlikely_val, true};
        if (s.condition.type().is_vector()) {
            // Devectorize the condition
            s.condition = and_condition_over_domain(s.condition, Scope<Interval>::empty_scope(), &s.tight);
        }
        internal_assert(s.condition.type().is_scalar()) << s.condition << "\n";
        simplifications.push_back(s);
    }

    void visit(const Min *op) {
        IRVisitor::visit(op);
        bool likely_a = has_likely_tag(op->a);
        bool likely_b = has_likely_tag(op->b);

        if (likely_b && !likely_a) {
            new_simplification(op->b <= op->a, op, op->b, op->a);
        } else if (likely_a && !likely_b) {
            new_simplification(op->a <= op->b, op, op->a, op->b);
        }
    }

    void visit(const Max *op) {
        IRVisitor::visit(op);
        bool likely_a = has_likely_tag(op->a);
        bool likely_b = has_likely_tag(op->b);

        if (likely_b && !likely_a) {
            new_simplification(op->b >= op->a, op, op->b, op->a);
        } else if (likely_a && !likely_b) {
            new_simplification(op->a >= op->b, op, op->a, op->b);
        }
    }

    void visit(const Select *op) {
        IRVisitor::visit(op);
        bool likely_t = has_likely_tag(op->true_value);
        bool likely_f = has_likely_tag(op->false_value);

        if (likely_t && !likely_f) {
            new_simplification(op->condition, op, op->true_value, op->false_value);
        } else if (likely_f && !likely_t) {
            new_simplification(!op->condition, op, op->false_value, op->true_value);
        }
    }

    void visit(const For *op) {
        vector<Simplification> old;
        old.swap(simplifications);
        IRVisitor::visit(op);

        // Relax all the new conditions using the loop bounds
        for (Simplification &s : simplifications) {
            Scope<Interval> varying;
            varying.push(op->name, Interval(op->min, op->min + op->extent - 1));
            s.condition = and_condition_over_domain(s.condition, varying, &s.tight);
        }

        simplifications.insert(simplifications.end(), old.begin(), old.end());
    }

    template<typename LetOrLetStmt>
    void visit_let(const LetOrLetStmt *op) {
        vector<Simplification> old;
        old.swap(simplifications);
        IRVisitor::visit(op);
        for (Simplification &s : simplifications) {
            if (expr_uses_var(s.condition, op->name)) {
                s.condition = Let::make(op->name, op->value, s.condition);
            }
        }
        simplifications.insert(simplifications.end(), old.begin(), old.end());
    }

    void visit(const LetStmt *op) {
        visit_let(op);
    }

    void visit(const Let *op) {
        visit_let(op);
    }
};

class MakeSimplifications : public IRMutator {
    using IRMutator::visit;

    const vector<Simplification> &simplifications;

public:

    MakeSimplifications(const vector<Simplification> &s) : simplifications(s) {}

    using IRMutator::mutate;
    Expr mutate(Expr e) {
        for (auto const &s : simplifications) {
            if (e.same_as(s.old_expr)) {
                return mutate(s.likely_value);
            }
        }
        return IRMutator::mutate(e);
    }

};

class PartitionLoops : public IRMutator {
    using IRMutator::visit;

    void visit(const For *op) {
        Stmt body = op->body;

        FindSimplifications finder;
        body.accept(&finder);

        debug(3) << "\n\n**** Partitioning loop over " << op->name << "\n";

        vector<Expr> min_vals, max_vals;
        vector<Simplification> middle_simps, prologue_simps, epilogue_simps;
        bool lower_bound_is_tight = true, upper_bound_is_tight = true;
        for (auto &s : finder.simplifications) {
            s.interval = solve_for_inner_interval(s.condition, op->name);
            if (s.tight) {
                Interval outer = solve_for_outer_interval(s.condition, op->name);
                s.tight &= equal(outer.min, s.interval.min) && equal(outer.max, s.interval.max);
            }

            debug(3) << "\nSimplification: \n"
                     << "  condition: " << s.condition << "\n"
                     << "  old: " << s.old_expr << "\n"
                     << "  new: " << s.likely_value << "\n"
                     << "  min: " << s.interval.min << "\n"
                     << "  max: " << s.interval.max << "\n";

            // Accept all non-empty intervals
            if (!interval_is_empty(s.interval)) {
                if (interval_has_lower_bound(s.interval)) {
                    Expr m = s.interval.min;
                    if (!s.tight) {
                        lower_bound_is_tight = false;
                    }
                    if (min_vals.empty()) {
                        min_vals.push_back(m);
                    } else if (equal(m, min_vals.back())) {
                        // We already have this min val
                    } else {
                        // This is a new distinct min val
                        min_vals.push_back(m);
                        lower_bound_is_tight = false;
                    }
                }
                if (interval_has_upper_bound(s.interval)) {
                    Expr m = s.interval.max;
                    if (!s.tight) {
                        upper_bound_is_tight = false;
                    }
                    if (max_vals.empty()) {
                        max_vals.push_back(m);
                    } else if (equal(m, max_vals.back())) {
                        // We already have this max val
                    } else {
                        // This is a new distinct max val
                        max_vals.push_back(m);
                        upper_bound_is_tight = false;
                    }
                }

                // We'll apply this simplification to the
                // steady-state.
                middle_simps.push_back(s);
            }
        }

        // In general we can't simplify the prologue - it may run up
        // to after the epilogue starts for small images. However if
        // we can prove the epilogue starts after the prologue ends,
        // we're OK.
        bool can_simplify_prologue = true;
        for (Expr min_val : min_vals) {
            for (Expr max_val : max_vals) {
                if (!is_one(simplify(min_val <= max_val))) {
                    can_simplify_prologue = false;
                }
            }
        }

        // Find simplifications we can apply to the prologue and epilogue.
        for (auto const &s : middle_simps) {
            // If it goes down to minus infinity, we can also
            // apply it to the prologue
            if (can_simplify_prologue &&
                !interval_has_lower_bound(s.interval)) {
                prologue_simps.push_back(s);
            }

            // If it goes up to positive infinity, we can also
            // apply it to the epilogue
            if (!interval_has_upper_bound(s.interval)) {
                epilogue_simps.push_back(s);
            }

            // If our simplifications only contain one lower bound, and
            // it's tight, then the reverse rule can be applied to the
            // prologue.
            if (can_simplify_prologue &&
                interval_has_lower_bound(s.interval) &&
                lower_bound_is_tight) {
                internal_assert(s.tight);
                Simplification s2 = s;
                // This condition is never used (we already solved
                // for the interval), but it's nice for it to be
                // correct.
                s2.condition = !s2.condition;
                std::swap(s2.likely_value, s2.unlikely_value);
                prologue_simps.push_back(s2);
            }
            if (interval_has_upper_bound(s.interval) &&
                upper_bound_is_tight) {
                internal_assert(s.tight);
                Simplification s2 = s;
                s2.condition = !s2.condition;
                std::swap(s2.likely_value, s2.unlikely_value);
                epilogue_simps.push_back(s2);
            }
        }

        // Simplify each section of the loop.
        Stmt simpler_body = MakeSimplifications(middle_simps).mutate(body);
        Stmt prologue = MakeSimplifications(prologue_simps).mutate(body);
        Stmt epilogue = MakeSimplifications(epilogue_simps).mutate(body);

        bool make_prologue = !equal(prologue, simpler_body);
        bool make_epilogue = !equal(epilogue, simpler_body);

        // Recurse on the middle section.
        simpler_body = mutate(simpler_body);

        // Construct variables for the bounds of the simplified middle section
        Expr min_steady = op->min, max_steady = op->extent + op->min;
        Expr prologue_val, epilogue_val;
        string prologue_name = unique_name(op->name + ".prologue", false);
        string epilogue_name = unique_name(op->name + ".epilogue", false);

        if (make_prologue) {
            // They'll simplify better if you put them in lexicographic order
            std::sort(min_vals.begin(), min_vals.end(), IRDeepCompare());
            min_vals.push_back(op->min);
            prologue_val = std::accumulate(min_vals.begin() + 1, min_vals.end(), min_vals[0], Max::make);
            min_steady = Variable::make(Int(32), prologue_name);

            internal_assert(!expr_uses_var(prologue_val, op->name));
        }
        if (make_epilogue) {
            std::sort(max_vals.begin(), max_vals.end(), IRDeepCompare());
            max_vals.push_back(op->min + op->extent - 1);
            epilogue_val = std::accumulate(max_vals.begin() + 1, max_vals.end(), max_vals[0], Min::make) + 1;
            if (make_prologue) {
                epilogue_val = max(epilogue_val, prologue_val);
            }
            max_steady = Variable::make(Int(32), epilogue_name);

            internal_assert(!expr_uses_var(epilogue_val, op->name));
        }

        if (op->for_type == ForType::Serial) {
            stmt = For::make(op->name, min_steady, max_steady - min_steady,
                             op->for_type, op->device_api, simpler_body);

            if (make_prologue) {
                prologue = For::make(op->name, op->min, min_steady - op->min,
                                     op->for_type, op->device_api, prologue);
                //prologue = Block::make(Evaluate::make(print(op->name, " prologue")), prologue);
                stmt = Block::make(prologue, stmt);
            }
            if (make_epilogue) {
                epilogue = For::make(op->name, max_steady, op->min + op->extent - max_steady,
                                     op->for_type, op->device_api, epilogue);
                //epilogue = Block::make(Evaluate::make(print(op->name, " epilogue")), epilogue);
                stmt = Block::make(stmt, epilogue);
            }
        } else {
            Expr loop_var = Variable::make(Int(32), op->name);
            stmt = simpler_body;
            if (make_epilogue) {
                stmt = IfThenElse::make(loop_var < max_steady, stmt, epilogue);
            }
            if (make_prologue) {
                stmt = IfThenElse::make(loop_var < min_steady, prologue, stmt);
            }
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, stmt);
        }

        if (make_epilogue) {
            //epilogue_val = print(epilogue_val, op->name, "epilogue");
            stmt = LetStmt::make(epilogue_name, epilogue_val, stmt);
        }
        if (make_prologue) {
            //prologue_val = print(prologue_val, op->name, "prologue");
            stmt = LetStmt::make(prologue_name, prologue_val, stmt);
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
                Expr var = Variable::make(false_value.type(), var_name);
                expr = mutate(Select::make(a->a, Select::make(a->b, true_value, var), var));
                expr = Let::make(var_name, false_value, expr);
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

// Collapse selects back together
class CollapseSelects : public IRMutator {
    using IRMutator::visit;

    void visit(const Select *op) {
        const Select *t = op->true_value.as<Select>();
        const Select *f = op->false_value.as<Select>();

        if (t && equal(t->false_value, op->false_value)) {
            // select(a, select(b, t, f), f) -> select(a && b, t, f)
            expr = mutate(select(op->condition && t->condition, t->true_value, op->false_value));
        } else if (f && equal(op->true_value, f->true_value)) {
            // select(a, t, select(b, t, f)) -> select(a || b, t, f)
            expr = mutate(select(op->condition || f->condition, op->true_value, f->false_value));
        } else {
            IRMutator::visit(op);
        }
    }
};

/** Remove identity functions, even if they have side-effects. */
class StripIdentities : public IRMutator {
    using IRMutator::visit;

    void visit(const Call *op) {
        if (op->call_type == Call::Intrinsic &&
            op->name == Call::trace_expr) {
            expr = mutate(op->args[4]);
        } else if (op->call_type == Call::Intrinsic &&
                   (op->name == Call::return_second ||
                    op->name == Call::likely)) {
            expr = mutate(op->args.back());
        } else {
            IRMutator::visit(op);
        }
    }
};


/** Construct a sufficient condition for the visited stmt to be a no-op. */
class IsNoOp : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Store *op) {
        if (op->value.type().is_handle()) {
            condition = const_false();
        } else {
            // If the value being stored is the same as the value loaded,
            // this is a no-op
            debug(3) << "Considering store: " << Stmt(op) << "\n";
            Expr equivalent_load = Load::make(op->value.type(), op->name, op->index, Buffer(), Parameter());
            Expr is_no_op = equivalent_load == op->value;
            is_no_op = StripIdentities().mutate(is_no_op);
            debug(3) << "Anding condition over domain... " << is_no_op << "\n";
            is_no_op = and_condition_over_domain(is_no_op, Scope<Interval>::empty_scope(), &tight);
            condition = condition && is_no_op;
            debug(3) << "Condition is now " << condition << "\n";
        }
    }

    void visit(const For *op) {
        Expr old_condition = condition;
        op->body.accept(this);
        Scope<Interval> varying;
        varying.push(op->name, Interval(op->min, op->min + op->extent - 1));
        condition = simplify(common_subexpression_elimination(condition));
        debug(3) << "About to relax over " << op->name << " : " << condition << "\n";
        condition = and_condition_over_domain(condition, varying, &tight);
        debug(3) << "Relaxed: " << condition << "\n";
        condition = old_condition && (condition || simplify(op->extent <= 0));
    }

    void visit(const Call *op) {
        // Certain intrinsics that may appear in loops have side-effects. Most notably: image_store.
        if (op->call_type == Call::Intrinsic &&
            (op->name == Call::rewrite_buffer ||
             op->name == Call::image_store ||
             op->name == Call::copy_memory)) {
            condition = const_false();
        } else {
            IRVisitor::visit(op);
        }
    }

    void visit(const IfThenElse *op) {
        Expr old_condition = condition;
        condition = const_true();
        op->then_case.accept(this);

    }

    template<typename LetOrLetStmt>
    void visit_let(const LetOrLetStmt *op) {
        IRVisitor::visit(op);
        if (expr_uses_var(condition, op->name)) {
            condition = Let::make(op->name, op->value, condition);
        }
    }

    void visit(const LetStmt *op) {
        visit_let(op);
    }

    void visit(const Let *op) {
        visit_let(op);
    }

public:
    Expr condition = const_true();

    /** If this is still true after visiting the Stmt, then the
     * condition is sufficient and necessary, not just sufficient. */
    bool tight = true;
};


class SimplifyUsingBounds : public IRMutator {
    struct ContainingLoop {
        string var;
        Interval i;
    };
    vector<ContainingLoop> containing_loops;

    using IRMutator::visit;

    // Can we prove a condition over the non-rectangular domain of the for loops we're in?
    bool provably_true_over_domain(Expr test) {
        bool tight = true;
        for (size_t i = containing_loops.size(); i > 0; i--) {
            // Because the domain is potentially non-rectangular, we
            // need to take each variable one-by-one, simplifying in
            // between to allow for cancellations of the bounds of
            // inner loops with outer loop variables.
            auto loop = containing_loops[i-1];
            if (loop.i.min.same_as(loop.i.max) && expr_uses_var(test, loop.var)) {
                test = Let::make(loop.var, loop.i.min, test);
            } else {
                Scope<Interval> s;
                s.push(loop.var, loop.i);
                test = simplify(and_condition_over_domain(test, s, &tight));
            }
        }
        return is_one(test);
    }

    void visit(const Min *op) {
        if (!op->type.is_int() || op->type.bits < 32) {
            IRMutator::visit(op);
        } else {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            Expr test = a <= b;
            if (provably_true_over_domain(a <= b)) {
                expr = a;
            } else if (provably_true_over_domain(b <= a)) {
                expr = b;
            } else {
                expr = Min::make(a, b);
            }
        }
    }

    void visit(const Max *op) {
        if (!op->type.is_int() || op->type.bits < 32) {
            IRMutator::visit(op);
        } else {
            Expr a = mutate(op->a);
            Expr b = mutate(op->b);
            if (provably_true_over_domain(a >= b)) {
                expr = a;
            } else if (provably_true_over_domain(b >= a)) {
                expr = b;
            } else {
                expr = Max::make(a, b);
            }
        }
    }

    template<typename Cmp>
    void visit_cmp(const Cmp *op) {
        IRMutator::visit(op);
        if (provably_true_over_domain(expr)) {
            expr = make_one(op->type);
        } else if (provably_true_over_domain(!expr)) {
            expr = make_zero(op->type);
        }
    }

    void visit(const LE *op) {
        visit_cmp(op);
    }

    void visit(const LT *op) {
        visit_cmp(op);
    }

    void visit(const GE *op) {
        visit_cmp(op);
    }

    void visit(const GT *op) {
        visit_cmp(op);
    }

    void visit(const EQ *op) {
        visit_cmp(op);
    }

    void visit(const NE *op) {
        visit_cmp(op);
    }

    template<typename StmtOrExpr, typename LetStmtOrLet>
    StmtOrExpr visit_let(const LetStmtOrLet *op) {
        Expr value = mutate(op->value);
        containing_loops.push_back({op->name, {value, value}});
        StmtOrExpr body = mutate(op->body);
        containing_loops.pop_back();
        return LetStmtOrLet::make(op->name, value, body);
    }

    void visit(const Let *op) {
        expr = visit_let<Expr, Let>(op);
    }

    void visit(const LetStmt *op) {
        stmt = visit_let<Stmt, LetStmt>(op);
    }

    void visit(const For *op) {
        // Simplify the loop bounds.
        Expr min = mutate(op->min);
        Expr extent = mutate(op->extent);
        containing_loops.push_back({op->name, {min, min + extent - 1}});
        Stmt body = mutate(op->body);
        containing_loops.pop_back();
        stmt = For::make(op->name, min, extent, op->for_type, op->device_api, body);
    }
public:
    SimplifyUsingBounds(const string &v, const Interval &i) {
        containing_loops.push_back({v, i});
    }
};

class TrimNoOps : public IRMutator {
    using IRMutator::visit;

    Scope<Interval> loop_bounds;

    void visit(const For *op) {
        Stmt body = mutate(op->body);

        IsNoOp is_no_op;
        body.accept(&is_no_op);
        debug(3) << "Condition is " << is_no_op.condition << "\n";
        is_no_op.condition = simplify(simplify(common_subexpression_elimination(is_no_op.condition)));

        debug(3) << "Simplified condition is " << is_no_op.condition << "\n";

        if (is_one(is_no_op.condition)) {
            // This loop is definitely useless
            stmt = Evaluate::make(0);
            return;
        } else if (is_zero(is_no_op.condition)) {
            // This loop is definitely needed
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
            return;
        }

        // The condition is something interesting. Try to see if we
        // can trim the loop bounds over which the loop does
        // something.
        Interval i = solve_for_outer_interval(!is_no_op.condition, op->name);

        if (interval_is_everything(i)) {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
            return;
        }

        debug(3) << "Interval is: " << i.min << ", " << i.max << "\n";

        // Simplify the body to take advantage of the fact that the
        // loop range is now truncated
        body = simplify(SimplifyUsingBounds(op->name, i).mutate(body));

        string new_min_name = unique_name(op->name + ".new_min", false);
        string new_max_name = unique_name(op->name + ".new_max", false);
        string old_max_name = unique_name(op->name + ".old_max", false);
        Expr new_min_var = Variable::make(Int(32), new_min_name);
        Expr new_max_var = Variable::make(Int(32), new_max_name);
        Expr old_max_var = Variable::make(Int(32), old_max_name);

        // Convert max to max-plus-one
        if (interval_has_upper_bound(i)) {
            i.max = i.max + 1;
        }

        // Truncate the loop bounds to the region over which it's not
        // a no-op.
        Expr old_max = op->min + op->extent;
        Expr new_min, new_max;
        if (interval_has_lower_bound(i)) {
            new_min = clamp(i.min, op->min, old_max_var);
        } else {
            new_min = op->min;
        }
        if (interval_has_upper_bound(i)) {
            new_max = clamp(i.max, new_min_var, old_max_var);
        } else {
            new_max = old_max;
        }

        Expr new_extent = new_max - new_min;

        stmt = For::make(op->name, new_min_var, new_extent, op->for_type, op->device_api, body);
        stmt = LetStmt::make(new_max_name, new_max, stmt);
        stmt = LetStmt::make(new_min_name, new_min, stmt);
        stmt = LetStmt::make(old_max_name, old_max, stmt);

    }
};

}

Stmt partition_loops(Stmt s) {
    s = MarkClampedRampsAsLikely().mutate(s);
    s = ExpandSelects().mutate(s);
    s = PartitionLoops().mutate(s);
    s = RenormalizeGPULoops().mutate(s);
    s = RemoveLikelyTags().mutate(s);
    s = CollapseSelects().mutate(s);
    s = TrimNoOps().mutate(s);
    return s;
}

}
}
