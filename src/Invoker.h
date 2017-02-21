#ifndef HALIDE_INVOKER_H_
#define HALIDE_INVOKER_H_

#include <memory>
#include <string>
#include <vector>

#include "Buffer.h"
#include "Func.h"
#include "Generator.h"

/*
TODO:
-- better error msgs in general
-- streamlined generate()+schedule() syntax, for jitting purposes?
-- We use existing stuff from the Stub code; if this replaces Stubs completely,
   some of that could be streamlined a lot (e.g. StubInputBuffer, StubOutputBuffer)
*/

namespace Halide {

class Invoker;

namespace Internal {

// std::integer_sequence (etc) is standard in C++14 but not C++11, but
// can be written in C++11. This is a quick-n-dirty version that could 
// probably be improved:

template<typename T, T... Ints> 
struct integer_sequence {
    static constexpr size_t size() { return sizeof...(Ints); }
};

template<typename T> 
struct next_integer_sequence;

template<typename T, T... Ints> 
struct next_integer_sequence<integer_sequence<T, Ints...>> {
    using type = integer_sequence<T, Ints..., sizeof...(Ints)>;
};

template<typename T, T I, T N> 
struct make_integer_sequence_helper {
    using type = typename next_integer_sequence<typename make_integer_sequence_helper<T, I+1, N>::type>::type;
};

template<typename T, T N> 
struct make_integer_sequence_helper<T, N, N> {
    using type = integer_sequence<T>;
};

template<typename T, T N>
using make_integer_sequence = typename make_integer_sequence_helper<T, 0, N>::type;

template<size_t... Ints>
using index_sequence = integer_sequence<size_t, Ints...>;

template<size_t N>
using make_index_sequence = make_integer_sequence<size_t, N>;

class Returnable;

class Realizeable {
    friend class Returnable;
    Func f;
    Target t;

    Realizeable(Func f, Target t) : f(f), t(t) {}
public:
    Realization realize(std::vector<int32_t> sizes) {
        return f.realize(sizes, t);
    }

    template <typename... Args>
    Realization realize(Args&&... args) {
        return f.realize(std::forward<Args>(args)..., t);
    }

    template<typename Dst>
    void realize(Dst dst) {
        f.realize(dst, t);
    }

    // Move, yes; Copy; No.
    Realizeable(Realizeable &&) = default;
    Realizeable &operator=(Realizeable &&) = default;

    Realizeable(const Realizeable &) = delete;
    Realizeable &operator=(const Realizeable &) = delete;
};

// Can't specialize on return type, so we workaround: return a temporary
// struct that has multiple cast overloads to do the dirty work for us.
class Returnable {
    friend class ::Halide::Invoker;
    const std::shared_ptr<GeneratorBase> generator;
    const size_t i;

    Returnable(std::shared_ptr<GeneratorBase> generator, size_t i) : generator(generator), i(i) {}
public:
    // Output<Func> -> Func
    operator Func() const {
        auto *out = generator->filter_outputs.at(i);
        const auto k = out->kind();
        user_assert(k == Internal::IOKind::Buffer || k == Internal::IOKind::Function)
            << "Output type mismatch for " << out->name();
        user_assert(!out->is_array())
            << "Output type mismatch for " << out->name();
        return out->funcs().at(0);
    }

    // Output<Func[]> -> vector<Func>
    operator std::vector<Func>() const {
        auto *out = generator->filter_outputs.at(i);
        const auto k = out->kind();
        user_assert(k == Internal::IOKind::Buffer || k == Internal::IOKind::Function)
            << "Output type mismatch for " << out->name();
        user_assert(out->is_array())
            << "Output type mismatch for " << out->name();
        return out->funcs();
    }

    // Output<Buffer<>> -> StubOutputBuffer (i.e., only assignment to another Output<Buffer<>>)
    operator StubOutputBuffer<>() const {
        auto *out = generator->filter_outputs.at(i);
        const auto k = out->kind();
        user_assert(k == Internal::IOKind::Buffer)
            << "Output type mismatch for " << out->name();
        user_assert(!out->is_array())
            << "Output type mismatch for " << out->name();
        return StubOutputBuffer<>(out->funcs().at(0), generator);
    }

    // Output<AnyNonArray> -> Realizeable
    operator Realizeable() const {
        auto *out = generator->filter_outputs.at(i);
        user_assert(!out->is_array())
            << "Output type mismatch for " << out->name();
        return {out->funcs().at(0), generator->get_target()};
    }

    // Output<AnyArray[]> -> Realizeable
    Realizeable operator[](size_t j) const {
        auto *out = generator->filter_outputs.at(i);
        user_assert(out->is_array())
            << "Output type mismatch for " << out->name();
        return {out->funcs().at(j), generator->get_target()};
    }

    // Convenience wrappers to allow calling invoker["output_name"].realize() directly
    Realization realize(std::vector<int32_t> sizes) {
        return ((Realizeable)(*this)).realize(sizes);
    }

    template <typename... Args>
    Realization realize(Args&&... args) {
        return ((Realizeable)(*this)).realize(args...);
    }

    template<typename Dst>
    void realize(Dst dst) {
        ((Realizeable)(*this)).realize(dst);
    }

    // Move, yes; Copy; No.
    Returnable(Returnable &&) = default;
    Returnable &operator=(Returnable &&) = default;

    Returnable(const Returnable &) = delete;
    Returnable &operator=(const Returnable &) = delete;
};



}  // namespace Internal

class Invoker {
private:
    using GeneratorBase = Halide::Internal::GeneratorBase;
    template<typename T = void> using StubInputBuffer = Halide::Internal::StubInputBuffer<T>;
    using StubInput = Halide::Internal::StubInput;
    using InputVec = std::vector<std::vector<StubInput>>;
    template<typename T = void> using StubOutputBuffer = Halide::Internal::StubOutputBuffer<T>;
    template<size_t... Ints> using index_sequence = Halide::Internal::index_sequence<Ints...>;
    template<size_t N> using make_index_sequence = Halide::Internal::make_index_sequence<N>;
    using Returnable = Internal::Returnable;

    std::string name;
    Target target;
    std::shared_ptr<GeneratorBase> generator;

    // Allow Buffer<> if:
    // -- we are assigning it to an Input<Buffer<>> (with compatible type and dimensions),
    // causing the Input<Buffer<>> to become a precompiled buffer in the generated code.
    // -- we are assigningit to an Input<Func>, in which case we just Func-wrap the Buffer<>.
    template<typename T>
    std::vector<StubInput> get_input(size_t i, const Buffer<T> &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(!in->is_array())
            << "Input type mismatch for " << in->name();
        const auto k = in->kind();
        if (k == Internal::IOKind::Buffer) {
            Halide::Buffer<> b = arg;
            StubInputBuffer<> sib(b);
            StubInput si(sib);
            return {si};
        } else if (k == Internal::IOKind::Function) {
            Halide::Func f(arg.name() + "_im");
            f(Halide::_) = arg(Halide::_);
            StubInput si(f);
            return {si};
        } else {
            user_assert(0)
                << "Input type mismatch for " << in->name();
            return {};
        }
    }

    // Allow Input<Buffer<>> if:
    // -- we are assigning it to another Input<Buffer<>> (with compatible type and dimensions),
    // allowing us to simply pipe a parameter from an enclosing Generator to the Invoker.
    // -- we are assigningit to an Input<Func>, in which case we just Func-wrap the Input<Buffer<>>.
    template<typename T>
    std::vector<StubInput> get_input(size_t i, const GeneratorInput<Buffer<T>> &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(!in->is_array())
            << "Input type mismatch for " << in->name();
        const auto k = in->kind();
        if (k == Internal::IOKind::Buffer) {
            StubInputBuffer<> sib = arg;
            StubInput si(sib);
            return {si};
        } else if (k == Internal::IOKind::Function) {
            Halide::Func f = arg.funcs().at(0);
            StubInput si(f);
            return {si};
        } else {
            user_assert(0)
                << "Input type mismatch for " << in->name();
            return {};
        }
    }

    // Allow Func iff we are assigning it to an Input<Func> (with compatible type and dimensions).
    std::vector<StubInput> get_input(size_t i, const Func &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Function)
            << "Input type mismatch for " << in->name();
        user_assert(!in->is_array())
            << "Input type mismatch for " << in->name();
        Halide::Func f = arg;
        StubInput si(f);
        return {si};
    }

    // Allow vector<Func> iff we are assigning it to an Input<Func[]> (with compatible type and dimensions).
    std::vector<StubInput> get_input(size_t i, const std::vector<Func> &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Function)
            << "Input type mismatch for " << in->name();
        user_assert(in->is_array())
            << "Input type mismatch for " << in->name();
        std::vector<StubInput> siv;
        for (const auto &f : arg) {
            siv.emplace_back(f);
        }
        return siv;
    }

    // Expr must be Input<Scalar>.
    std::vector<StubInput> get_input(size_t i, const Expr &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Scalar)
            << "Input type mismatch for " << in->name();
        user_assert(!in->is_array())
            << "Input type mismatch for " << in->name();
        StubInput si(arg);
        return {si};
    }

    // (Array form)
    std::vector<StubInput> get_input(size_t i, const std::vector<Expr> &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Scalar)
            << "Input type mismatch for " << in->name();
        user_assert(in->is_array())
            << "Input type mismatch for " << in->name();
        std::vector<StubInput> siv;
        for (const auto &value : arg) {
            siv.emplace_back(value);
        }
        return siv;
    }

    // Any other type must be convertible to Expr and must be associated with an Input<Scalar>.
    // Use is_integral and is_floating_point since some Expr conversions are explicit.
    template<typename T,
             typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value>::type * = nullptr>
    std::vector<StubInput> get_input(size_t i, const T &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Scalar)
            << "Input type mismatch for " << in->name();
        user_assert(!in->is_array())
            << "Input type mismatch for " << in->name();
        // We must use an explicit Expr() ctor to preserve the type
        Expr e(arg);
        StubInput si(e);
        return {si};
    }

    // (Array form)
    template<typename T,
             typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value>::type * = nullptr>
    std::vector<StubInput> get_input(size_t i, const std::vector<T> &arg) {
        auto *in = generator->filter_inputs.at(i);
        user_assert(in->kind() == Internal::IOKind::Scalar)
            << "Input type mismatch for " << in->name();
        user_assert(in->is_array())
            << "Input type mismatch for " << in->name();
        std::vector<StubInput> siv;
        for (const auto &value : arg) {
            // We must use an explicit Expr() ctor to preserve the type;
            // otherwise, implicit conversions can downgrade (e.g.) float -> int
            Expr e(value);
            siv.emplace_back(e);
        }
        return siv;
    }

    template<typename... Args, size_t... Indices>
    InputVec build_inputs(const std::tuple<const Args &...>& t, index_sequence<Indices...>) { 
        return {get_input(Indices, std::get<Indices>(t))...};
    }

    // TODO: linear search is suboptimal
    size_t output_name_to_index(const std::string &name) const {
        size_t i = 0;
        for (auto *out : generator->filter_outputs) {
            if (out->name() == name) {
                break;
            }
            i += 1;
        }
        user_assert(i < generator->filter_outputs.size()) << "Output " << name << " not found.\n";
        return i;
    }

    // TODO: linear search is suboptimal
    Internal::GeneratorParamBase *find_generator_param(const std::string &name) const {
        for (auto *gp : generator->generator_params) {
            if (gp->name == name) {
                return gp;
            }
        }
        user_assert(0) << "GeneratorParam " << name << " not found.\n";
        return nullptr;
    }

// TODO: this is horrible. Surely must be a better way to do multimethod dispatch here.
// Quick-n-dirty to validate look & feel of API for now.
#define SET_PARAM_IMPL(TYPE) \
    static void set_param_impl(Internal::GeneratorParamBase *gp, const TYPE &value) { \
        bool settable = gp->set_from_##TYPE(value); \
        user_assert(settable) << "GeneratorParam " << gp->name << " is not settable with type " #TYPE "."; \
    }

    SET_PARAM_IMPL(bool)
    SET_PARAM_IMPL(int8_t)
    SET_PARAM_IMPL(int16_t)
    SET_PARAM_IMPL(int32_t)
    SET_PARAM_IMPL(int64_t)
    SET_PARAM_IMPL(uint8_t)
    SET_PARAM_IMPL(uint16_t)
    SET_PARAM_IMPL(uint32_t)
    SET_PARAM_IMPL(uint64_t)
    SET_PARAM_IMPL(float)
    SET_PARAM_IMPL(double)
    SET_PARAM_IMPL(LoopLevel)
    SET_PARAM_IMPL(Target)
    SET_PARAM_IMPL(Type)

#undef SET_PARAM_IMPL

    // Overloads to allow setting-from-string. The char* overload is necessary
    // to disambiguate vs bool.
    static void set_param_impl(Internal::GeneratorParamBase *gp, const std::string &value) {
        gp->set_from_string(value);
    }
    static void set_param_impl(Internal::GeneratorParamBase *gp, const char *value) {
        gp->set_from_string(value);
    }

public:
    Invoker() = default;

    Invoker(const GeneratorContext* context, 
              const std::string &name) 
        : name(name), target(context->get_target()), generator(Halide::Internal::GeneratorRegistry::create(name, {})) {
        generator->target.set(target);
    }

    Invoker(const GeneratorContext& context, const std::string &name) 
        : Invoker(&context, name) {}

    template<typename T>
    Invoker &&set_generator_param(const std::string &name, const T &value) {
        user_assert(name != "target") << "Cannot call set_generator_param(\"target\")";
        user_assert(!generator->generate_called) 
            << "Cannot call set_generator_param() for an Invoker after its generate() method has been called.";
        user_assert(!generator->schedule_called) 
            << "Cannot call set_generator_param() for an Invoker after its schedule() method has been called.";
        auto *gp = find_generator_param(name);
        user_assert(!gp->is_schedule_param())
            << "Cannot call set_generator_param() on a ScheduleParam.";
        set_param_impl(gp, value);
        // This is pretty weird, so let's comment: we can't return an lvalue-ref, because
        // we are uncopyable and thus have deleted our copy-assignment operator; however,
        // we are movable, so we can return an rvalue-ref just fine. This is unusual, but
        // legal (AFAIK), as long as the returned rvalue-ref is known to remain valid in the
        // calling context, which is the cased here. (Note also that we use an explicit cast rather
        // than std::move in order to avoid compiler warnings about "never use std::move on returned values".)
        return (Invoker &&)(*this);
    }

    template <typename... Args>
    Invoker &&generate(const Args &...args) {
        user_assert(sizeof...(Args) == generator->filter_inputs.size());
        user_assert(!generator->generate_called) 
            << "Cannot call generate() multiple times for the same Invoker.";
        InputVec inputs = build_inputs(std::forward_as_tuple<const Args &...>(args...), make_index_sequence<sizeof...(Args)>{});
        generator->set_inputs(inputs);
        generator->call_generate();
        return (Invoker &&)(*this);
    }

    template<typename T>
    Invoker &&set_schedule_param(const std::string &name, const T &value) {
        user_assert(generator->generate_called) 
            << "Cannot call set_schedule_param() for an Invoker before its generate() method has been called.";
        user_assert(!generator->schedule_called) 
            << "Cannot call set_schedule_param() for an Invoker after its schedule() method has been called.";
        auto *gp = find_generator_param(name);
        user_assert(gp->is_schedule_param())
            << "Cannot call set_schedule_param() on a GeneratorParam.";
        set_param_impl(gp, value);
        return (Invoker &&)(*this);
    }

    Invoker &&schedule() {
        user_assert(generator->generate_called) << "Cannot call schedule() before generate().";
        user_assert(!generator->schedule_called)  << "Cannot call schedule() multiple times for the same Invoker.";
        generator->call_schedule();
        return (Invoker &&)(*this);
    }

    Returnable operator[](size_t i) const {
        user_assert(generator->generate_called) << "Cannot get outputs until generate() is called.";
        return Returnable(generator, i);
    }

    Returnable operator[](const std::string &name) const {
        user_assert(generator->generate_called) 
            << "Cannot get outputs until generate() is called.";
        return (*this)[output_name_to_index(name)];
    }

    Realization realize(std::vector<int32_t> sizes) {
        // TODO: this check probably belongs in GeneratorBase::produce_pipeline
        user_assert(generator->schedule_called) << "Cannot call realize() until after calling schedule().";
        return generator->produce_pipeline().realize(sizes, target);
    }

    // Only enable if none of the args are Realization; otherwise we can incorrectly
    // select this method instead of the Realization-as-outparam variant
    template <typename... Args, typename std::enable_if<Internal::NoRealizations<Args...>::value>::type * = nullptr>
    Realization realize(Args&&... args) {
        // TODO: this check probably belongs in GeneratorBase::produce_pipeline
        user_assert(generator->schedule_called) << "Cannot call realize() until after calling schedule().";
        return generator->produce_pipeline().realize(std::forward<Args>(args)..., target);
    }

    void realize(Realization r) {
        // TODO: this check probably belongs in GeneratorBase::produce_pipeline
        user_assert(generator->schedule_called) << "Cannot call realize() until after calling schedule().";
        generator->produce_pipeline().realize(r, target);
    }

    // Move, yes; Copy; No.
    Invoker(Invoker &&) = default;
    Invoker &operator=(Invoker &&) = default;

    Invoker(const Invoker &) = delete;
    Invoker &operator=(const Invoker &) = delete;
};

}  // namespace Halide

#endif  // HALIDE_INVOKER_H_
