#include "StoreForwarding.h"
#include "IROperator.h"
#include "IRMutator.h"
#include "IREquality.h"
#include "Debug.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

struct BufferAccess {
    string buffer;
    Expr index;

    BufferAccess() {}
    BufferAccess(const string &buffer, Expr index) : buffer(buffer), index(index) {}
    BufferAccess(const Load *load) : buffer(load->name), index(load->index) {}
    BufferAccess(const Store *store) : buffer(store->name), index(store->index) {}

    bool operator == (const BufferAccess &r) const {
        return buffer == r.buffer && equal(index, r.index);
    }

    bool operator < (const BufferAccess &r) const {
        if (buffer < r.buffer) {
            return true;
        } else if (buffer > r.buffer) {
            return false;
        }

        static IRDeepCompare comparer;
        return comparer(index, r.index);
    }
};

std::ostream &operator << (std::ostream &os, const BufferAccess &a) {
    return os << a.buffer << "[" << a.index << "]";
}

struct LoadStore {
    BufferAccess load, store;
};

// Find all of the trivial load->store sequences in the Stmt.
class GatherTrivialStores : public IRVisitor {
    using IRVisitor::visit;

    void visit(const ProducerConsumer *op) {
        // Don't enter the production of another func.
    }

    void visit(const Store *op) {
        const Load *value = op->value.as<Load>();
        if (value) {
            LoadStore load_store;
            load_store.load = BufferAccess(value);
            load_store.store = BufferAccess(op);
            result.push_back(load_store);
        }
    }

public:
    vector<LoadStore> result;
};

// This visitor verifies that if a buffer access were to be replaced,
// either the replacement fully replaces the target, or that loads and
// stores to the same buffer
class IsReplacementComplete : public IRVisitor {
    using IRVisitor::visit;

    BufferAccess target;

    void visit(const Load *op) {
        if (op->name != target.buffer) {
            IRVisitor::visit(op);
            return;
        }

        // For replacement to be complete, the indices must be constant.
        if (is_const(op->index) && is_const(target.index)) {
            IRVisitor::visit(op);
            return;
        }

        success = false;
    }

    void visit(const Store *op) {
        if (op->name != target.buffer) {
            IRVisitor::visit(op);
            return;
        }

        // For replacement to be complete, the indices must be constant.
        if (is_const(op->index) && is_const(target.index)) {
            IRVisitor::visit(op);
            return;
        }

        success = false;
    }

public:
    bool success;

    IsReplacementComplete(const BufferAccess &target) : target(target), success(true) {}
};

bool is_replacement_complete(Stmt stmt, BufferAccess target) {
    IsReplacementComplete is_complete(target);
    stmt.accept(&is_complete);
    return is_complete.success;
}

class ReplaceBufferAccesses : public IRMutator {
    using IRVisitor::visit;

    void visit(const Store *op) {
        BufferAccess store_to(op);
        // If this is a store to a buffer we are forwarding, change the store.
        for (const auto &i : replacements) {
            if (i.first == store_to) {
                stmt = Store::make(i.second.buffer, mutate(op->value), i.second.index);
                return;
            }
        }
        IRMutator::visit(op);
    }

    void visit(const Load *op) {
        BufferAccess load(op);
        for (const auto &i : replacements) {
            if (i.first == load) {
                expr = Load::make(op->type, i.second.buffer, i.second.index);
                return;
            }
        }
        IRMutator::visit(op);
    }

    const std::map<BufferAccess, BufferAccess> &replacements;

public:
    ReplaceBufferAccesses(const std::map<BufferAccess, BufferAccess> &replacements) : replacements(replacements) {}
};

Stmt replace_buffer_accesses(Stmt stmt, const std::map<BufferAccess, BufferAccess> &replacements) {
    ReplaceBufferAccesses replacer(replacements);
    return replacer.mutate(stmt);
}

// Because storage folding runs before simplification, it's useful to
// at least substitute in constants before running it, and also simplify the RHS of Let Stmts.
class StoreForwarding : public IRMutator {
    using IRMutator::visit;

    void visit(const ProducerConsumer *op) {
        Stmt produce = mutate(op->produce);
        Stmt update;
        if (op->update.defined()) {
            update = mutate(op->update);
        }
        Stmt consume = mutate(op->consume);

        if (produce.same_as(op->produce) &&
            update.same_as(op->update) &&
            consume.same_as(op->consume)) {
            stmt = op;
        } else {
            stmt = ProducerConsumer::make(op->name, produce, update, consume);
        }

        // Get the trivial stores from the consumer of this Func.
        GatherTrivialStores trivial_stores;
        consume.accept(&trivial_stores);

        // Only forward the trivial stores that can be proven
        // safe. This means that we can only forward stores that:
        // - Do not
        std::map<BufferAccess, BufferAccess> replacements;
        for (const LoadStore &i : trivial_stores.result) {
            if (is_replacement_complete(stmt, i.load)) {
                replacements[i.load] = i.store;
            }
        }

        //for (const LoadStore &i : trivial_stores.result) {
        //    debug(0) << i.store << " = " << i.load << "\n";
        //}

        stmt = replace_buffer_accesses(stmt, replacements);
    }

};

Stmt forward_stores(Stmt s, const std::vector<Function> &outputs) {
    s = StoreForwarding().mutate(s);
    return s;
}

}
}
