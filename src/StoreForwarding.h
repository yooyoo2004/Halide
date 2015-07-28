#ifndef HALIDE_STORE_FORWARDING_H
#define HALIDE_STORE_FORWARDING_H

/** \file
 * Defines the lowering pass that flattens multi-dimensional storage
 * into single-dimensional array access
 */

#include <map>

#include "IR.h"

namespace Halide {
namespace Internal {

/** Take a statement with multi-dimensional Realize, Provide, and Call
 * nodes, and turn it into a statement with single-dimensional
 * Allocate, Store, and Load nodes respectively. */
Stmt forward_stores(Stmt s, const std::vector<Function> &outputs);

}
}

#endif
