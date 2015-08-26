#ifndef HALIDE_LIFT_ALLOCATIONS_H
#define HALIDE_LIFT_ALLOCATIONS_H

/** \file Defines a lowering pass that lifts non-constant allocations
 * out of inner loops where possible.
 */

#include "IR.h"

namespace Halide {
namespace Internal {

/** Move Allocate nodes outside of loops where possible. */
Stmt lift_allocations(Stmt s);

}
}

#endif
