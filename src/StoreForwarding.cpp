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
};

std::ostream &operator << (std::ostream &os, const BufferAccess &a) {
    return os << a.buffer << "[" << a.index << "]";
}

struct LoadStore {
    BufferAccess load, store;
};

class GatherTrivialStores : public IRVisitor {
    using IRVisitor::visit;

public:
    vector<LoadStore> result;

    void visit(const ProducerConsumer *op) {
        // Don't enter the production of another func.
    }

    void visit(const For *op) {
        // Don't enter the production of another func.
    }

    void visit(const IfThenElse *op) {
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
};

class ReplaceStores : public IRMutator {
    using IRVisitor::visit;

public:
    const vector<LoadStore> &stores_to_replace;

    ReplaceStores(const vector<LoadStore> &stores_to_replace) : stores_to_replace(stores_to_replace) {}

    void visit(const Store *op) {
        BufferAccess store_to(op);
        // If this is a store to a buffer we are forwarding, change the store.
        for (const LoadStore &i : stores_to_replace) {
            if (i.load == store_to) {
                stmt = Store::make(i.store.buffer, mutate(op->value), i.store.index);
                return;
            }
        }
        IRMutator::visit(op);
    }

    void visit(const Load *op) {
        BufferAccess load(op);
        for (const LoadStore &i : stores_to_replace) {
            if (i.load == load) {
                expr = Load::make(op->type, i.store.buffer, i.store.index, op->image, op->param);
                return;
            }
        }
        IRMutator::visit(op);
    }
};

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

        //for (const LoadStore &i : trivial_stores.result) {
        //    debug(0) << i.store << " = " << i.load << "\n";
        //}

        ReplaceStores replace_stores(trivial_stores.result);
        stmt = replace_stores.mutate(stmt);
    }

};

Stmt forward_stores(Stmt s, const std::vector<Function> &outputs) {
    s = StoreForwarding().mutate(s);
    return s;
}

}
}
