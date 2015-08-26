#include <list>
#include <vector>
#include <map>

#include "LiftAllocations.h"
#include "IRVisitor.h"
#include "IRMutator.h"
#include "ExprUsesVar.h"
#include "IROperator.h"
#include "IREquality.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::string;
using std::list;
using std::vector;
using std::map;

// IRVisitor to check if some IR contains something we shouldn't lift
// an allocation around.
class ContainsLiftingBarrier : public IRVisitor {
public:
    bool found = false;

private:
    using IRVisitor::visit;

    // Don't lift past assert statements, they might prevent something
    // bad from happening in the lifted code.
    void visit(const AssertStmt *) {
        found = true;
    }

    // Don't lift past things with potential side effects.
    void visit(const Evaluate *op) {
        found = true;
    }

    void visit(const Call *op) {
        if (op->call_type == Call::Extern) {
            found = true;
        } else {
            IRVisitor::visit(op);
        }
    }
};

bool contains_lifting_barrier(Stmt s) {
    ContainsLiftingBarrier check;
    s.accept(&check);
    return check.found;
}

// IR visitor to check if an Expr is liftable.
class CanLift : public IRVisitor {
public:
    bool result = true;

private:
    using IRVisitor::visit;

    // Don't lift an expression with a load.
    void visit(const Load *) {
        result = false;
    }

    // Don't lift things that might have side effects.
    void visit(const Call *op) {
        if (op->call_type == Call::Extern) {
            result = false;
        } else {
            IRVisitor::visit(op);
        }
    }
};

bool can_lift(Expr e) {
    CanLift check;
    e.accept(&check);
    return check.result;
}

// Merge two allocations into one. They must have the same dimensionality.
Stmt merge_allocations(const Allocate *a, const Allocate *b) {
    Expr condition = a->condition || b->condition;

    internal_assert(a->extents.size() == b->extents.size());
    std::vector<Expr> extents;
    for (size_t i = 0; i < a->extents.size(); i++) {
        extents.push_back(max(a->extents[i], b->extents[i]));
    }

    internal_assert(equal(a->new_expr, b->new_expr));

    return Allocate::make(a->name, a->type, extents, condition, a->body, a->new_expr, a->free_function);
}

string var_defined(Stmt s) {
    const LetStmt *l = s.as<LetStmt>();
    if (l) {
        return l->name;
    }
    const Allocate *a = s.as<Allocate>();
    if (a) {
        return a->name;
    }
    return "";
}

// Check if an allocation or let depends on a var.
bool allocate_depends_on(const Allocate *a, const string &value) {
    if (expr_uses_var(a->condition, value)) {
        return true;
    }
    for (Expr i : a->extents) {
        if (expr_uses_var(i, value)) {
            return true;
        }
    }
    if (a->new_expr.defined() && expr_uses_var(a->new_expr, value)) {
        return true;
    }
    return false;
}

bool let_depends_on(const LetStmt *l, const string &value) {
    if (expr_uses_var(l->value, value)) {
        return true;
    }
    return false;
}

// Check if a depends on b.
bool depends_on(Stmt stmt, Stmt on) {
    debug(4) << var_defined(stmt) << " " << var_defined(on) << "\n";
    string var = var_defined(on);

    const LetStmt *l = stmt.as<LetStmt>();
    if (l) {
        return let_depends_on(l, var);
    }
    const Allocate *a = stmt.as<Allocate>();
    if (a) {
        return allocate_depends_on(a, var);
    }
    return false;
}

// Mutator to lift allocations (and lets) out of loops they do not depend on where possible.
class LiftAllocations : public IRMutator {
    using IRMutator::visit;

    // This is a list of Stmts (LetStmt or Allocate nodes) that are
    // waiting to be lifted.
    list<Stmt> to_lift;

    // Rewrap a single statement with a let or alloc.
    static Stmt rewrap(Stmt s, Stmt let_or_alloc) {
        const LetStmt *l = let_or_alloc.as<LetStmt>();
        if (l) {
            debug(4) << "Rewrapped let " << l->name << "\n";
            return LetStmt::make(l->name, l->value, s);
        }
        const Allocate *a = let_or_alloc.as<Allocate>();
        if (a) {
            debug(4) << "Rewrapped allocate " << a->name << "\n";
            return Allocate::make(a->name, a->type, a->extents, a->condition, s, a->new_expr, a->free_function);
        }

        internal_error << "Stmt to rewrap was not an Allocate or LetStmt.\n" << let_or_alloc;
        return s;
    }

    // Unfortunately, this is identical to rewrap_if, but using
    // rewrap_if with the appropriate predicate results in recursive
    // template expansion.
    Stmt rewrap_dependent(Stmt s, list<Stmt>::iterator end, Stmt needed) {
        debug(4) << "Rewrapping dependents on " << var_defined(needed) << "\n";
        for (auto i = to_lift.begin(); i != end;) {
            if (depends_on(*i, needed)) {
                debug(4) << var_defined(*i) << " depends on " << var_defined(needed) << "\n";
                s = rewrap_dependent(s, i, *i);
                s = rewrap(s, *i);
                i = to_lift.erase(i);
            } else {
                debug(4) << var_defined(*i) << " does not depend on " << var_defined(needed) << "\n";
                i++;
            }
        }
        return s;
    }

    // Rewrap all pending lifted statements up to end that satisfy the predicate.
    template <typename F>
    Stmt rewrap_if(Stmt s, list<Stmt>::iterator end, F predicate) {
        for (auto i = to_lift.begin(); i != end;) {
            if (predicate(*i)) {
                // If we are going to rewrap this stmt, (recursively)
                // rewrap anything that this depends on first.
                s = rewrap_dependent(s, i, *i);
                s = rewrap(s, *i);
                i = to_lift.erase(i);
            } else {
                i++;
            }
        }
        return s;
    }

    // Rewrap all pending lifted statements that satisfy the predicate.
    template <typename F>
    Stmt rewrap_if(Stmt s, F predicate) {
        return rewrap_if(s, to_lift.end(), predicate);
    }

    // Rewrap all pending lifted statements up to end.
    Stmt rewrap_all(Stmt s, list<Stmt>::iterator end) {
        for (auto i = to_lift.begin(); i != end; i++) {
            s = rewrap(s, *i);
        }
        to_lift.erase(to_lift.begin(), end);
        return s;
    }

public:
    // Rewrap all pending lifted statements.
    Stmt rewrap_all(Stmt s) {
        return rewrap_all(s, to_lift.end());
    }

private:
    // Mutate a statement without lifting anything past this
    // statement. Any pending lifting statements from inside s are
    // wrapped around s.
    Stmt mutate_with_barrier(Stmt s) {
        if (!s.defined()) {
            return s;
        }

        // Remmeber the first stmt to rewrap outside the barrier.
        list<Stmt>::iterator outer = to_lift.begin();

        // Mutate the stmt
        s = mutate(s);
        // Rewrap with everything we found inside s.
        s = rewrap_all(s, outer);

        return s;
    }

    void visit(const For *loop) {
        debug(4) << "Entering loop " << loop->name << "\n";

        // Remember stmts that are already pending lifting, so we
        // don't bother to try lifting them out of this loop.
        auto outer = to_lift.begin();

        Stmt body = mutate(loop->body);

        body = rewrap_if(body, outer, [&](Stmt i) {
           debug(4) << "Trying to lift out of " << loop->name << "\n";

           const LetStmt *let = i.as<LetStmt>();
           if (let) {
               // If the let depends on the loop variable, we can't lift it any further.
               if (let_depends_on(let, loop->name)) {
                   debug(4) << "Not lifting let " << let->name << " because it depends on loop " << loop->name << "\n";
                   return true;
               }

               // This let can be lifted outside this loop.
               debug(4) << "Lifting let " << let->name << " with value " << let->value << " out of " << loop->name << "\n";
               return false;
           }

           const Allocate *alloc = i.as<Allocate>();
           if (alloc) {
               // If the loop is parallel, we can't lift the allocation out of it.
               if (loop->for_type == ForType::Parallel) {
                   debug(4) << "Not lifting allocation " << alloc->name << " out of parallel loop " << loop->name << "\n";
                   return true;
               }

               if (allocate_depends_on(alloc, loop->name)) {
                   debug(4) << "Not lifting allocation " << alloc->name << " because it depends on loop " << loop->name << "\n";
                   return true;
               }

               // This allocation can be lifted outside this loop.
               debug(4) << "Lifting allocation " << alloc->name << " out of loop " << loop->name << "\n";
               return false;
           }

           return false;
        });

        if (!body.same_as(loop->body)) {
            stmt = For::make(loop->name, loop->min, loop->extent, loop->for_type,
                             loop->device_api, body);
        } else {
            internal_assert(outer == to_lift.begin()) << "Lifted something out of an unchanged loop body.\n";
            stmt = loop;
        }

        debug(4) << "Exiting loop " << loop->name << "\n";
    }

    void visit(const Allocate *alloc) {
        // Keep the allocation body here, put the allocation on the
        // list of allocations to be lifted. Because update
        // definitions or loop unrolling may instantiate multiple
        // realizations of a Func, we need to merge all the
        // allocations with the same name.
        bool found = false;
        for (auto i = to_lift.begin(); i != to_lift.end(); i++) {
            const Allocate *ai = i->as<Allocate>();
            if (ai && ai->name == alloc->name) {
                *i = merge_allocations(alloc, ai);
                found = true;
                break;
            }
        }
        if (!found) {
            to_lift.push_front(alloc);
        }
        stmt = mutate(alloc->body);
    }

    // This helps us rename lets with identical names to something unique.
    map<string, int> let_names;

    void visit(const LetStmt *let) {
        Stmt body = let->body;
        Stmt new_let = let;
        list<Stmt>::iterator to_lift_it = to_lift.end();
        for (auto i = to_lift.begin(); i != to_lift.end(); i++) {
            const LetStmt *li = i->as<LetStmt>();
            if (li && li->name == let->name) {
                if (equal(li->value, let->value)) {
                    to_lift_it = i;
                } else {
                    // Uniquify the name.
                    int suffix = let_names[let->name]++;
                    string name = let->name + ".lifted" + std::to_string(suffix);
                    body = substitute(let->name, Variable::make(let->value.type(), name), body);
                    to_lift.push_front(LetStmt::make(name, let->value, body));
                    to_lift_it = to_lift.begin();
                }
                break;
            }
        }
        if (to_lift_it == to_lift.end()) {
            to_lift.push_front(let);
            to_lift_it = to_lift.begin();
        }

        stmt = mutate(body);

        // If the let contains something we can't lift, rewrap it now.
        if (!can_lift(let->value)) {
            // Rewrap the dependents first.
            stmt = rewrap_dependent(stmt, to_lift_it, *to_lift_it);
            stmt = rewrap(stmt, *to_lift_it);
            to_lift.erase(to_lift_it);
        }
    }

    void visit(const IfThenElse *op) {
        // Don't lift things out of conditional statements.
        Stmt then_case = mutate_with_barrier(op->then_case);
        Stmt else_case = mutate_with_barrier(op->else_case);

        if (!then_case.same_as(op->then_case) || !else_case.same_as(op->else_case)) {
            stmt = IfThenElse::make(op->condition, then_case, else_case);
        } else {
            stmt = op;
        }
    }

    void visit(const Block *op) {
        Stmt first = op->first;
        Stmt rest = op->rest;

        // Don't lift past lifting barriers.
        if (contains_lifting_barrier(first)) {
            first = mutate_with_barrier(first);
            rest = mutate_with_barrier(rest);
        } else if (rest.defined() && contains_lifting_barrier(rest)) {
            first = mutate(first);
            rest = mutate_with_barrier(rest);
        } else {
            IRMutator::visit(op);
            return;
        }

        if (!first.same_as(op->first) || !rest.same_as(op->rest)) {
            stmt = Block::make(first, rest);
        } else {
            stmt = op;
        }
    }
};

Stmt lift_allocations(Stmt s) {
//    return s;

    LiftAllocations lift_allocations;
    s = lift_allocations.mutate(s);

    // Rewrap any leftover allocations.
    s = lift_allocations.rewrap_all(s);

    return s;
}

}
}
