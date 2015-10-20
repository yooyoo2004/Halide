#ifndef SOLVE_H
#define SOLVE_H

/** Defines methods for solving equations. */

#include "IR.h"
#include "Scope.h"
#include "Bounds.h"

namespace Halide {
namespace Internal {

/** Attempts to collect all instances of a variable in an expression
 * tree and place it as far to the left as possible, and as far up the
 * tree as possible (i.e. outside most parentheses). If the expression
 * is an equality or comparison, this 'solves' the equation. Returns
 * an undefined expression on failure. */
EXPORT Expr solve_expression(Expr e, const std::string &variable, const Scope<Expr> &scope = Scope<Expr>::empty_scope());

/** Find the smallest interval such that the condition is only true
 * inside of it, and definitely false outside of it. */
EXPORT Interval solve_for_outer_interval(Expr c, const std::string &variable);

/** Find the largest interval such that the condition is definitely
 * true inside of it, and might be true or false outside of it. */
EXPORT Interval solve_for_inner_interval(Expr c, const std::string &variable);

/** Check properties of the intervals returned by solve_for_*_interval. */
// @{
EXPORT bool interval_has_upper_bound(const Interval &i);
EXPORT bool interval_has_lower_bound(const Interval &i);
EXPORT bool interval_is_everything(const Interval &i);
EXPORT bool interval_is_empty(const Interval &i);
// @}

EXPORT void solve_test();

}
}

#endif
