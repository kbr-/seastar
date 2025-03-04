/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/apply.hh>
#include <seastar/core/task.hh>
#include <seastar/core/preempt.hh>
#include <seastar/core/thread_impl.hh>
#include <stdexcept>
#include <atomic>
#include <memory>
#include <type_traits>
#include <assert.h>
#include <cstdlib>
#include <seastar/core/function_traits.hh>
#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/gcc6-concepts.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar {

/// \defgroup future-module Futures and Promises
///
/// \brief
/// Futures and promises are the basic tools for asynchronous
/// programming in seastar.  A future represents a result that
/// may not have been computed yet, for example a buffer that
/// is being read from the disk, or the result of a function
/// that is executed on another cpu.  A promise object allows
/// the future to be eventually resolved by assigning it a value.
///
/// \brief
/// Another way to look at futures and promises are as the reader
/// and writer sides, respectively, of a single-item, single use
/// queue.  You read from the future, and write to the promise,
/// and the system takes care that it works no matter what the
/// order of operations is.
///
/// \brief
/// The normal way of working with futures is to chain continuations
/// to them.  A continuation is a block of code (usually a lamdba)
/// that is called when the future is assigned a value (the future
/// is resolved); the continuation can then access the actual value.
///

/// \defgroup future-util Future Utilities
///
/// \brief
/// These utilities are provided to help perform operations on futures.


/// \addtogroup future-module
/// @{

template <class... T>
class promise;

template <class... T>
class future;

template <typename... T>
class shared_future;

/// \brief Creates a \ref future in an available, value state.
///
/// Creates a \ref future object that is already resolved.  This
/// is useful when it is determined that no I/O needs to be performed
/// to perform a computation (for example, because the data is cached
/// in some buffer).
template <typename... T, typename... A>
future<T...> make_ready_future(A&&... value);

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This is useful when no I/O needs to be performed to perform
/// a computation (for example, because the connection is closed and
/// we cannot read from it).
template <typename... T>
future<T...> make_exception_future(std::exception_ptr value) noexcept;

/// \cond internal
void engine_exit(std::exception_ptr eptr = {});

void report_failed_future(const std::exception_ptr& ex) noexcept;
/// \endcond

/// \brief Exception type for broken promises
///
/// When a promise is broken, i.e. a promise object with an attached
/// continuation is destroyed before setting any value or exception, an
/// exception of `broken_promise` type is propagated to that abandoned
/// continuation.
struct broken_promise : std::logic_error {
    broken_promise();
};

namespace internal {
// It doesn't seem to be possible to use std::tuple_element_t with an empty tuple. There is an static_assert in it that
// fails the build even if it is in the non enabled side of std::conditional.
template <typename... T>
struct get0_return_type {
    using type = void;
    static type get0(std::tuple<T...> v) { }
};

template <typename T0, typename... T>
struct get0_return_type<T0, T...> {
    using type = T0;
    static type get0(std::tuple<T0, T...> v) { return std::get<0>(std::move(v)); }
};

/// \brief Wrapper for keeping uninitialized values of non default constructible types.
///
/// This is similar to a std::optional<T>, but it doesn't know if it is holding a value or not, so the user is
/// responsible for calling constructors and destructors.
///
/// The advantage over just using a union directly is that this uses inheritance when possible and so benefits from the
/// empty base optimization.
template <typename T, bool is_trivial_class>
struct uninitialized_wrapper_base;

template <typename T>
struct uninitialized_wrapper_base<T, false> {
    union any {
        any() {}
        ~any() {}
        T value;
    } _v;

public:
    void uninitialized_set(T v) {
        new (&_v.value) T(std::move(v));
    }
    T& uninitialized_get() {
        return _v.value;
    }
    const T& uninitialized_get() const {
        return _v.value;
    }
};

template <typename T> struct uninitialized_wrapper_base<T, true> : private T {
    void uninitialized_set(T v) {
        new (this) T(std::move(v));
    }
    T& uninitialized_get() {
        return *this;
    }
    const T& uninitialized_get() const {
        return *this;
    }
};

template <typename T>
constexpr bool can_inherit =
#ifdef _LIBCPP_VERSION
// We expect std::tuple<> to be trivially constructible and
// destructible. That is not the case with libc++
// (https://bugs.llvm.org/show_bug.cgi?id=41714).  We could avoid this
// optimization when using libc++ and relax the asserts, but
// inspection suggests that std::tuple<> is trivial, it is just not
// marked as such.
        std::is_same<std::tuple<>, T>::value ||
#endif
        (std::is_trivially_destructible<T>::value && std::is_trivially_constructible<T>::value &&
                std::is_class<T>::value && !std::is_final<T>::value);

// The objective is to avoid extra space for empty types like std::tuple<>. We could use std::is_empty_v, but it is
// better to check that both the constructor and destructor can be skipped.
template <typename T>
struct uninitialized_wrapper
    : public uninitialized_wrapper_base<T, can_inherit<T>> {};

static_assert(std::is_empty<uninitialized_wrapper<std::tuple<>>>::value, "This should still be empty");
}

//
// A future/promise pair maintain one logical value (a future_state).
// There are up to three places that can store it, but only one is
// active at any time.
//
// - in the promise _local_state member variable
//
//   This is necessary because a promise is created first and there
//   would be nowhere else to put the value.
//
// - in the future _state variable
//
//   This is used anytime a future exists and then has not been called
//   yet. This guarantees a simple access to the value for any code
//   that already has a future.
//
// - in the task associated with the .then() clause (after .then() is called,
//   if a value was not set)
//
//
// The promise maintains a pointer to the state, which is modified as
// the state moves to a new location due to events (such as .then() or
// get_future being called) or due to the promise or future being
// moved around.
//

// non templated base class to reduce code duplication
struct future_state_base {
    static_assert(std::is_nothrow_copy_constructible<std::exception_ptr>::value,
                  "std::exception_ptr's copy constructor must not throw");
    static_assert(std::is_nothrow_move_constructible<std::exception_ptr>::value,
                  "std::exception_ptr's move constructor must not throw");
    static_assert(sizeof(std::exception_ptr) == sizeof(void*), "exception_ptr not a pointer");
    enum class state : uintptr_t {
         invalid = 0,
         future = 1,
         result = 2,
         exception_min = 3,  // or anything greater
    };
    union any {
        any() { st = state::future; }
        any(state s) { st = s; }
        void set_exception(std::exception_ptr&& e) {
            new (&ex) std::exception_ptr(std::move(e));
            assert(st >= state::exception_min);
        }
        any(std::exception_ptr&& e) {
            set_exception(std::move(e));
        }
        ~any() {}
        std::exception_ptr take_exception() {
            std::exception_ptr ret(std::move(ex));
            // Unfortunately in libstdc++ ~exception_ptr is defined out of line. We know that it does nothing for
            // moved out values, so we omit calling it. This is critical for the code quality produced for this
            // function. Without the out of line call, gcc can figure out that both sides of the if produce
            // identical code and merges them.if
            // We don't make any assumptions about other c++ libraries.
            // There is request with gcc to define it inline: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90295
#ifndef __GLIBCXX__
            ex.~exception_ptr();
#endif
            st = state::invalid;
            return ret;
        }
        any(any&& x) {
            if (x.st < state::exception_min) {
                st = x.st;
            } else {
                new (&ex) std::exception_ptr(x.take_exception());
            }
            x.st = state::invalid;
        }
        state st;
        std::exception_ptr ex;
    } _u;

    future_state_base() noexcept { }
    future_state_base(state st) noexcept : _u(st) { }
    future_state_base(std::exception_ptr&& ex) noexcept : _u(std::move(ex)) { }
    future_state_base(future_state_base&& x) noexcept : _u(std::move(x._u)) { }

    bool available() const noexcept { return _u.st == state::result || _u.st >= state::exception_min; }
    bool failed() const noexcept { return _u.st >= state::exception_min; }

    void set_to_broken_promise() noexcept;

    void set_exception(std::exception_ptr&& ex) noexcept {
        assert(_u.st == state::future);
        _u.set_exception(std::move(ex));
    }
    std::exception_ptr get_exception() && noexcept {
        assert(_u.st >= state::exception_min);
        // Move ex out so future::~future() knows we've handled it
        return _u.take_exception();
    }
    const std::exception_ptr& get_exception() const& noexcept {
        assert(_u.st >= state::exception_min);
        return _u.ex;
    }
};

struct ready_future_marker {};
struct exception_future_marker {};

/// \cond internal
template <typename... T>
struct future_state :  public future_state_base, private internal::uninitialized_wrapper<std::tuple<T...>> {
    static constexpr bool copy_noexcept = std::is_nothrow_copy_constructible<std::tuple<T...>>::value;
    static_assert(std::is_nothrow_move_constructible<std::tuple<T...>>::value,
                  "Types must be no-throw move constructible");
    static_assert(std::is_nothrow_destructible<std::tuple<T...>>::value,
                  "Types must be no-throw destructible");
    future_state() noexcept {}
    [[gnu::always_inline]]
    future_state(future_state&& x) noexcept : future_state_base(std::move(x)) {
        if (_u.st == state::result) {
            this->uninitialized_set(std::move(x.uninitialized_get()));
            x.uninitialized_get().~tuple();
        }
    }
    __attribute__((always_inline))
    ~future_state() noexcept {
        if (_u.st == state::result) {
            this->uninitialized_get().~tuple();
        } else if (_u.st >= state::exception_min) {
            _u.ex.~exception_ptr();
        }
    }
    future_state& operator=(future_state&& x) noexcept {
        this->~future_state();
        new (this) future_state(std::move(x));
        return *this;
    }
    template <typename... A>
    future_state(ready_future_marker, A&&... a) : future_state_base(state::result) {
        this->uninitialized_set(std::tuple<T...>(std::forward<A>(a)...));
    }
    template <typename... A>
    void set(A&&... a) {
        assert(_u.st == state::future);
        new (this) future_state(ready_future_marker(), std::forward<A>(a)...);
    }
    future_state(exception_future_marker m, std::exception_ptr&& ex) : future_state_base(std::move(ex)) { }
    std::tuple<T...> get_value() && noexcept {
        assert(_u.st == state::result);
        return std::move(this->uninitialized_get());
    }
    template<typename U = std::tuple<T...>>
    std::enable_if_t<std::is_copy_constructible<U>::value, U> get_value() const& noexcept(copy_noexcept) {
        assert(_u.st == state::result);
        return this->uninitialized_get();
    }
    std::tuple<T...> get() && {
        assert(_u.st != state::future);
        if (_u.st >= state::exception_min) {
            // Move ex out so future::~future() knows we've handled it
            std::rethrow_exception(std::move(*this).get_exception());
        }
        return std::move(this->uninitialized_get());
    }
    std::tuple<T...> get() const& {
        assert(_u.st != state::future);
        if (_u.st >= state::exception_min) {
            std::rethrow_exception(_u.ex);
        }
        return this->uninitialized_get();
    }
    void ignore() noexcept {
        assert(_u.st != state::future);
        this->~future_state();
        _u.st = state::invalid;
    }
    using get0_return_type = typename internal::get0_return_type<T...>::type;
    static get0_return_type get0(std::tuple<T...>&& x) {
        return internal::get0_return_type<T...>::get0(std::move(x));
    }
};

static_assert(sizeof(future_state<>) <= 8, "future_state<> is too large");
static_assert(sizeof(future_state<long>) <= 16, "future_state<long> is too large");

template <typename... T>
class continuation_base : public task {
protected:
    future_state<T...> _state;
    using future_type = future<T...>;
    using promise_type = promise<T...>;
public:
    continuation_base() = default;
    explicit continuation_base(future_state<T...>&& state) : _state(std::move(state)) {}
    void set_state(future_state<T...>&& state) {
        _state = std::move(state);
    }
    friend class promise<T...>;
    friend class future<T...>;
};

template <typename Func, typename... T>
struct continuation final : continuation_base<T...> {
    continuation(Func&& func, future_state<T...>&& state) : continuation_base<T...>(std::move(state)), _func(std::move(func)) {}
    continuation(Func&& func) : _func(std::move(func)) {}
    virtual void run_and_dispose() noexcept override {
        _func(std::move(this->_state));
        delete this;
    }
    Func _func;
};

namespace internal {

template <typename... T, typename U>
void set_callback(future<T...>& fut, std::unique_ptr<U> callback);

class future_base;

class promise_base {
protected:
    enum class urgent { no, yes };
    future_base* _future = nullptr;

    // This points to the future_state that is currently being
    // used. See comment above the future_state struct definition for
    // details.
    future_state_base* _state;

    std::unique_ptr<task> _task;

    promise_base(const promise_base&) = delete;
    promise_base(future_state_base* state) noexcept : _state(state) {}
    promise_base(promise_base&& x) noexcept;
    void check_during_destruction() noexcept;

    void operator=(const promise_base&) = delete;
    promise_base& operator=(promise_base&& x) noexcept {
        this->~promise_base();
        new (this) promise_base(std::move(x));
        return *this;
    }

    friend class future_base;
    template <typename... U> friend class seastar::future;
};
}

/// \brief promise - allows a future value to be made available at a later time.
///
/// \tparam T A list of types to be carried as the result of the associated future.
///           A list with two or more types is deprecated; use
///           \c promise<std::tuple<T...>> instead.
template <typename... T>
class promise : private internal::promise_base {
    future_state<T...> _local_state;

    future_state<T...>* get_state() {
        return static_cast<future_state<T...>*>(_state);
    }
    static constexpr bool copy_noexcept = future_state<T...>::copy_noexcept;
public:
    /// \brief Constructs an empty \c promise.
    ///
    /// Creates promise with no associated future yet (see get_future()).
    promise() noexcept : promise_base(&_local_state) {}

    /// \brief Moves a \c promise object.
    promise(promise&& x) noexcept;
    promise(const promise&) = delete;
    ~promise() noexcept {
        check_during_destruction();
    }
    promise& operator=(promise&& x) noexcept {
        this->~promise();
        new (this) promise(std::move(x));
        return *this;
    }
    void operator=(const promise&) = delete;

    /// \brief Gets the promise's associated future.
    ///
    /// The future and promise will be remember each other, even if either or
    /// both are moved.  When \c set_value() or \c set_exception() are called
    /// on the promise, the future will be become ready, and if a continuation
    /// was attached to the future, it will run.
    future<T...> get_future() noexcept;

    void set_urgent_state(future_state<T...>&& state) noexcept {
        if (_state) {
            *get_state() = std::move(state);
            make_ready<urgent::yes>();
        }
    }

    /// \brief Sets the promises value
    ///
    /// Forwards the arguments and makes them available to the associated
    /// future.  May be called either before or after \c get_future().
    ///
    /// The arguments can have either the types the promise is
    /// templated with, or a corresponding std::tuple. That is, given
    /// a promise<int, double>, both calls are valid:
    ///
    /// pr.set_value(42, 43.0);
    /// pr.set_value(std::tuple<int, double>(42, 43.0))
    template <typename... A>
    void set_value(A&&... a) {
        if (auto *s = get_state()) {
            s->set(std::forward<A>(a)...);
            make_ready<urgent::no>();
        }
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    void set_exception(std::exception_ptr&& ex) noexcept {
        if (_state) {
            _state->set_exception(std::move(ex));
            make_ready<urgent::no>();
        }
    }

    void set_exception(const std::exception_ptr& ex) noexcept {
        set_exception(std::exception_ptr(ex));
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    template<typename Exception>
    std::enable_if_t<!std::is_same<std::remove_reference_t<Exception>, std::exception_ptr>::value, void> set_exception(Exception&& e) noexcept {
        set_exception(make_exception_ptr(std::forward<Exception>(e)));
    }

#if SEASTAR_COROUTINES_TS
    void set_coroutine(future_state<T...>& state, task& coroutine) noexcept {
        _state = &state;
        _task = std::unique_ptr<task>(&coroutine);
    }
#endif
private:
    template <typename Func>
    void schedule(Func&& func) {
        auto tws = std::make_unique<continuation<Func, T...>>(std::move(func));
        _state = &tws->_state;
        _task = std::move(tws);
    }
    void schedule(std::unique_ptr<continuation_base<T...>> callback) {
        _state = &callback->_state;
        _task = std::move(callback);
    }
    template<urgent Urgent>
    __attribute__((always_inline))
    void make_ready() noexcept;

    template <typename... U>
    friend class future;

    friend struct future_state<T...>;
};

/// \brief Specialization of \c promise<void>
///
/// This is an alias for \c promise<>, for generic programming purposes.
/// For example, You may have a \c promise<T> where \c T can legally be
/// \c void.
template<>
class promise<void> : public promise<> {};

/// @}

/// \addtogroup future-util
/// @{


/// \brief Check whether a type is a future
///
/// This is a type trait evaluating to \c true if the given type is a
/// future.
///
template <typename... T> struct is_future : std::false_type {};

/// \cond internal
/// \addtogroup future-util
template <typename... T> struct is_future<future<T...>> : std::true_type {};

/// \endcond


/// \brief Converts a type to a future type, if it isn't already.
///
/// \return Result in member type 'type'.
template <typename T>
struct futurize;

template <typename T>
struct futurize {
    /// If \c T is a future, \c T; otherwise \c future<T>
    using type = future<T>;
    /// The promise type associated with \c type.
    using promise_type = promise<T>;
    /// The value tuple type associated with \c type
    using value_type = std::tuple<T>;

    /// Apply a function to an argument list (expressed as a tuple)
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    /// Apply a function to an argument list
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    /// Convert a value or a future to a future
    static inline type convert(T&& value) { return make_ready_future<T>(std::move(value)); }
    static inline type convert(type&& value) { return std::move(value); }

    /// Convert the tuple representation into a future
    static type from_tuple(value_type&& value);
    /// Convert the tuple representation into a future
    static type from_tuple(const value_type& value);

    /// Makes an exceptional future of type \ref type.
    template <typename Arg>
    static type make_exception_future(Arg&& arg);
};

/// \cond internal
template <>
struct futurize<void> {
    using type = future<>;
    using promise_type = promise<>;
    using value_type = std::tuple<>;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    static inline type from_tuple(value_type&& value);
    static inline type from_tuple(const value_type& value);

    template <typename Arg>
    static type make_exception_future(Arg&& arg);
};

template <typename... Args>
struct futurize<future<Args...>> {
    using type = future<Args...>;
    using promise_type = promise<Args...>;
    using value_type = std::tuple<Args...>;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept;

    static inline type from_tuple(value_type&& value);
    static inline type from_tuple(const value_type& value);

    static inline type convert(Args&&... values) { return make_ready_future<Args...>(std::move(values)...); }
    static inline type convert(type&& value) { return std::move(value); }

    template <typename Arg>
    static type make_exception_future(Arg&& arg);
};
/// \endcond

// Converts a type to a future type, if it isn't already.
template <typename T>
using futurize_t = typename futurize<T>::type;

/// @}

GCC6_CONCEPT(

template <typename T>
concept bool Future = is_future<T>::value;

template <typename Func, typename... T>
concept bool CanApply = requires (Func f, T... args) {
    f(std::forward<T>(args)...);
};

template <typename Func, typename Return, typename... T>
concept bool ApplyReturns = requires (Func f, T... args) {
    { f(std::forward<T>(args)...) } -> Return;
};

template <typename Func, typename... T>
concept bool ApplyReturnsAnyFuture = requires (Func f, T... args) {
    requires is_future<decltype(f(std::forward<T>(args)...))>::value;
};

)

/// \addtogroup future-module
/// @{
namespace internal {
class future_base {
protected:
    promise_base* _promise;
    future_base() noexcept : _promise(nullptr) {}
    future_base(promise_base* promise, future_state_base* state) noexcept : _promise(promise) {
        _promise->_future = this;
        _promise->_state = state;
    }

    future_base(future_base&& x, future_state_base* state) noexcept : _promise(x._promise) {
        if (auto* p = _promise) {
            x.detach_promise();
            p->_future = this;
            p->_state = state;
        }
    }
    ~future_base() noexcept {
        if (_promise) {
            detach_promise();
        }
    }

    promise_base* detach_promise() noexcept {
        _promise->_state = nullptr;
        _promise->_future = nullptr;
        return std::exchange(_promise, nullptr);
    }

    friend class promise_base;
};

template <bool IsVariadic>
struct warn_variadic_future {
    // Non-varidic case, do nothing
    void check_deprecation() {}
};


// Note: placing the deprecated attribute on the class specialization has no effect.
template <>
struct warn_variadic_future<true> {
    // Variadic case, has deprecation attribute
    [[deprecated("Variadic future<> with more than one template parmeter is deprecated, replace with future<std::tuple<...>>")]]
    void check_deprecation() {}
};

}

/// \brief A representation of a possibly not-yet-computed value.
///
/// A \c future represents a value that has not yet been computed
/// (an asynchronous computation).  It can be in one of several
/// states:
///    - unavailable: the computation has not been completed yet
///    - value: the computation has been completed successfully and a
///      value is available.
///    - failed: the computation completed with an exception.
///
/// methods in \c future allow querying the state and, most importantly,
/// scheduling a \c continuation to be executed when the future becomes
/// available.  Only one such continuation may be scheduled.
///
/// \tparam T A list of types to be carried as the result of the future,
///           similar to \c std::tuple<T...>. An empty list (\c future<>)
///           means that there is no result, and an available future only
///           contains a success/failure indication (and in the case of a
///           failure, an exception).
///           A list with two or more types is deprecated; use
///           \c future<std::tuple<T...>> instead.
template <typename... T>
class future : private internal::future_base, internal::warn_variadic_future<(sizeof...(T) > 1)> {
    future_state<T...> _state;
    static constexpr bool copy_noexcept = future_state<T...>::copy_noexcept;
private:
    future(promise<T...>* pr) noexcept : future_base(pr, &_state), _state(std::move(pr->_local_state)) { }
    template <typename... A>
    future(ready_future_marker m, A&&... a) : _state(m, std::forward<A>(a)...) { }
    future(exception_future_marker m, std::exception_ptr ex) noexcept : _state(m, std::move(ex)) { }
    [[gnu::always_inline]]
    explicit future(future_state<T...>&& state) noexcept
            : _state(std::move(state)) {
        this->check_deprecation();
    }
    promise<T...>* detach_promise() {
        return static_cast<promise<T...>*>(future_base::detach_promise());
    }
    template <typename Func>
    void schedule(Func&& func) {
        if (_state.available() || !_promise) {
            if (__builtin_expect(!_state.available() && !_promise, false)) {
                abandoned();
            }
            ::seastar::schedule(std::make_unique<continuation<Func, T...>>(std::move(func), std::move(_state)));
        } else {
            assert(_promise);
            detach_promise()->schedule(std::move(func));
        }
    }

    [[gnu::always_inline]]
    future_state<T...> get_available_state() noexcept {
        if (_promise) {
            detach_promise();
        }
        return std::move(_state);
    }

    [[gnu::noinline]]
    future<T...> rethrow_with_nested() {
        if (!failed()) {
            return make_exception_future<T...>(std::current_exception());
        } else {
            //
            // Encapsulate the current exception into the
            // std::nested_exception because the current libstdc++
            // implementation has a bug requiring the value of a
            // std::throw_with_nested() parameter to be of a polymorphic
            // type.
            //
            std::nested_exception f_ex;
            try {
                get();
            } catch (...) {
                std::throw_with_nested(f_ex);
            }
            __builtin_unreachable();
        }
    }

    // Used when there is to attempt to attach a continuation or a thread to a future
    // that was abandoned by its promise.
    [[gnu::cold]] [[gnu::noinline]]
    void abandoned() noexcept {
        _state.set_to_broken_promise();
    }

    template<typename... U>
    friend class shared_future;
public:
    /// \brief The data type carried by the future.
    using value_type = std::tuple<T...>;
    /// \brief The data type carried by the future.
    using promise_type = promise<T...>;
    /// \brief Moves the future into a new object.
    [[gnu::always_inline]]
    future(future&& x) noexcept : future_base(std::move(x), &_state), _state(std::move(x._state)) { }
    future(const future&) = delete;
    future& operator=(future&& x) noexcept {
        this->~future();
        new (this) future(std::move(x));
        return *this;
    }
    void operator=(const future&) = delete;
    __attribute__((always_inline))
    ~future() {
        if (failed()) {
            report_failed_future(_state.get_exception());
        }
    }
    /// \brief gets the value returned by the computation
    ///
    /// Requires that the future be available.  If the value
    /// was computed successfully, it is returned (as an
    /// \c std::tuple).  Otherwise, an exception is thrown.
    ///
    /// If get() is called in a \ref seastar::thread context,
    /// then it need not be available; instead, the thread will
    /// be paused until the future becomes available.
    [[gnu::always_inline]]
    std::tuple<T...> get() {
        if (!_state.available()) {
            do_wait();
        }
        return get_available_state().get();
    }

    [[gnu::always_inline]]
     std::exception_ptr get_exception() {
        return get_available_state().get_exception();
    }

    /// Gets the value returned by the computation.
    ///
    /// Similar to \ref get(), but instead of returning a
    /// tuple, returns the first value of the tuple.  This is
    /// useful for the common case of a \c future<T> with exactly
    /// one type parameter.
    ///
    /// Equivalent to: \c std::get<0>(f.get()).
    typename future_state<T...>::get0_return_type get0() {
        return future_state<T...>::get0(get());
    }

    /// Wait for the future to be available (in a seastar::thread)
    ///
    /// When called from a seastar::thread, this function blocks the
    /// thread until the future is availble. Other threads and
    /// continuations continue to execute; only the thread is blocked.
    void wait() noexcept {
        if (!_state.available()) {
            do_wait();
        }
    }
private:
    class thread_wake_task final : public continuation_base<T...> {
        thread_context* _thread;
        future* _waiting_for;
    public:
        thread_wake_task(thread_context* thread, future* waiting_for)
                : _thread(thread), _waiting_for(waiting_for) {
        }
        virtual void run_and_dispose() noexcept override {
            _waiting_for->_state = std::move(this->_state);
            thread_impl::switch_in(_thread);
            // no need to delete, since this is always allocated on
            // _thread's stack.
        }
    };
    void do_wait() noexcept {
        if (__builtin_expect(!_promise, false)) {
            abandoned();
            return;
        }
        auto thread = thread_impl::get();
        assert(thread);
        thread_wake_task wake_task{thread, this};
        detach_promise()->schedule(std::unique_ptr<continuation_base<T...>>(&wake_task));
        thread_impl::switch_out(thread);
    }

public:
    /// \brief Checks whether the future is available.
    ///
    /// \return \c true if the future has a value, or has failed.
    [[gnu::always_inline]]
    bool available() const noexcept {
        return _state.available();
    }

    /// \brief Checks whether the future has failed.
    ///
    /// \return \c true if the future is availble and has failed.
    [[gnu::always_inline]]
    bool failed() noexcept {
        return _state.failed();
    }

    /// \brief Schedule a block of code to run when the future is ready.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the result of this future's
    /// computation as parameters.  The return value of the function becomes
    /// the return value of then(), itself as a future; this allows then()
    /// calls to be chained.
    ///
    /// If the future failed, the function is not called, and the exception
    /// is propagated into the return value of then().
    ///
    /// \param func - function to be called when the future becomes available,
    ///               unless it has failed.
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(T&&...)>>>
    GCC6_CONCEPT( requires ::seastar::CanApply<Func, T...> )
    Result
    then(Func&& func) noexcept {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_impl(std::move(func));
#else
        using futurator = futurize<std::result_of_t<Func(T&&...)>>;
        return then_impl(noncopyable_function<Result (T&&...)>([func = std::forward<Func>(func)] (T&&... args) mutable {
            return futurator::apply(func, std::forward_as_tuple(std::move(args)...));
        }));
#endif
    }

private:

    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(T&&...)>>>
    Result
    then_impl(Func&& func) noexcept {
        using futurator = futurize<std::result_of_t<Func(T&&...)>>;
        if (available() && !need_preempt()) {
            if (failed()) {
                return futurator::make_exception_future(get_available_state().get_exception());
            } else {
                return futurator::apply(std::forward<Func>(func), get_available_state().get_value());
            }
        }
        typename futurator::promise_type pr;
        auto fut = pr.get_future();
        // If there is a std::bad_alloc in schedule() there is nothing that can be done about it, we cannot break future
        // chain by returning ready future while 'this' future is not ready. The noexcept will call std::terminate if
        // that happens.
        [&] () noexcept {
            memory::disable_failure_guard dfg;
            schedule([pr = std::move(pr), func = std::forward<Func>(func)] (future_state<T...>&& state) mutable {
                if (state.failed()) {
                    pr.set_exception(std::move(state).get_exception());
                } else {
                    futurator::apply(std::forward<Func>(func), std::move(state).get_value()).forward_to(std::move(pr));
                }
            });
        } ();
        return fut;
    }

public:
    /// \brief Schedule a block of code to run when the future is ready, allowing
    ///        for exception handling.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the this future as a parameter;
    /// it will be in an available state.  The return value of the function becomes
    /// the return value of then_wrapped(), itself as a future; this allows
    /// then_wrapped() calls to be chained.
    ///
    /// Unlike then(), the function will be called for both value and exceptional
    /// futures.
    ///
    /// \param func - function to be called when the future becomes available,
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(future)>>>
    GCC6_CONCEPT( requires ::seastar::CanApply<Func, future> )
    Result
    then_wrapped(Func&& func) noexcept {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_wrapped_impl(std::move(func));
#else
        using futurator = futurize<std::result_of_t<Func(future)>>;
        return then_wrapped_impl(noncopyable_function<Result (future)>([func = std::forward<Func>(func)] (future f) mutable {
            return futurator::apply(std::forward<Func>(func), std::move(f));
        }));
#endif
    }

private:

    template <typename Func, typename Result = futurize_t<std::result_of_t<Func(future)>>>
    Result
    then_wrapped_impl(Func&& func) noexcept {
        using futurator = futurize<std::result_of_t<Func(future)>>;
        if (available() && !need_preempt()) {
            return futurator::apply(std::forward<Func>(func), future(get_available_state()));
        }
        typename futurator::promise_type pr;
        auto fut = pr.get_future();
        // If there is a std::bad_alloc in schedule() there is nothing that can be done about it, we cannot break future
        // chain by returning ready future while 'this' future is not ready. The noexcept will call std::terminate if
        // that happens.
        [&] () noexcept {
            memory::disable_failure_guard dfg;
            schedule([pr = std::move(pr), func = std::forward<Func>(func)] (future_state<T...>&& state) mutable {
                futurator::apply(std::forward<Func>(func), future(std::move(state))).forward_to(std::move(pr));
            });
        } ();
        return fut;
    }

public:
    /// \brief Satisfy some \ref promise object with this future as a result.
    ///
    /// Arranges so that when this future is resolve, it will be used to
    /// satisfy an unrelated promise.  This is similar to scheduling a
    /// continuation that moves the result of this future into the promise
    /// (using promise::set_value() or promise::set_exception(), except
    /// that it is more efficient.
    ///
    /// \param pr a promise that will be fulfilled with the results of this
    /// future.
    void forward_to(promise<T...>&& pr) noexcept {
        if (_state.available()) {
            pr.set_urgent_state(std::move(_state));
        } else {
            *detach_promise() = std::move(pr);
        }
    }



    /**
     * Finally continuation for statements that require waiting for the result.
     * I.e. you need to "finally" call a function that returns a possibly
     * unavailable future. The returned future will be "waited for", any
     * exception generated will be propagated, but the return value is ignored.
     * I.e. the original return value (the future upon which you are making this
     * call) will be preserved.
     *
     * If the original return value or the callback return value is an
     * exceptional future it will be propagated.
     *
     * If both of them are exceptional - the std::nested_exception exception
     * with the callback exception on top and the original future exception
     * nested will be propagated.
     */
    template <typename Func>
    GCC6_CONCEPT( requires ::seastar::CanApply<Func> )
    future<T...> finally(Func&& func) noexcept {
        return then_wrapped(finally_body<Func, is_future<std::result_of_t<Func()>>::value>(std::forward<Func>(func)));
    }


    template <typename Func, bool FuncReturnsFuture>
    struct finally_body;

    template <typename Func>
    struct finally_body<Func, true> {
        Func _func;

        finally_body(Func&& func) : _func(std::forward<Func>(func))
        { }

        future<T...> operator()(future<T...>&& result) {
            using futurator = futurize<std::result_of_t<Func()>>;
            return futurator::apply(_func).then_wrapped([result = std::move(result)](auto f_res) mutable {
                if (!f_res.failed()) {
                    return std::move(result);
                } else {
                    try {
                        f_res.get();
                    } catch (...) {
                        return result.rethrow_with_nested();
                    }
                    __builtin_unreachable();
                }
            });
        }
    };

    template <typename Func>
    struct finally_body<Func, false> {
        Func _func;

        finally_body(Func&& func) : _func(std::forward<Func>(func))
        { }

        future<T...> operator()(future<T...>&& result) {
            try {
                _func();
                return std::move(result);
            } catch (...) {
                return result.rethrow_with_nested();
            }
        };
    };

    /// \brief Terminate the program if this future fails.
    ///
    /// Terminates the entire program is this future resolves
    /// to an exception.  Use with caution.
    future<> or_terminate() noexcept {
        return then_wrapped([] (auto&& f) {
            try {
                f.get();
            } catch (...) {
                engine_exit(std::current_exception());
            }
        });
    }

    /// \brief Discards the value carried by this future.
    ///
    /// Converts the future into a no-value \c future<>, by
    /// ignoring any result.  Exceptions are propagated unchanged.
    future<> discard_result() noexcept {
        return then([] (T&&...) {});
    }

    /// \brief Handle the exception carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception,
    /// handle_exception(func) replaces the exception with the value
    /// returned by func. The exception is passed (as a std::exception_ptr)
    /// as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    ///
    /// The idiom fut.discard_result().handle_exception(...) can be used
    /// to handle an exception (if there is one) without caring about the
    /// successful value; Because handle_exception() is used here on a
    /// future<>, the handler function does not need to return anything.
    template <typename Func>
    /* Broken?
    GCC6_CONCEPT( requires ::seastar::ApplyReturns<Func, future<T...>, std::exception_ptr>
                    || (sizeof...(T) == 0 && ::seastar::ApplyReturns<Func, void, std::exception_ptr>)
                    || (sizeof...(T) == 1 && ::seastar::ApplyReturns<Func, T..., std::exception_ptr>)
    ) */
    future<T...> handle_exception(Func&& func) noexcept {
        using func_ret = std::result_of_t<Func(std::exception_ptr)>;
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T...> {
            if (!fut.failed()) {
                return make_ready_future<T...>(fut.get());
            } else {
                return futurize<func_ret>::apply(func, fut.get_exception());
            }
        });
    }

    /// \brief Handle the exception of a certain type carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception of a type that
    /// provided callback receives as a parameter, handle_exception(func) replaces
    /// the exception with the value returned by func. The exception is passed (by
    /// reference) as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    /// If exception, that future holds, does not match func parameter type
    /// it is propagated as is.
    template <typename Func>
    future<T...> handle_exception_type(Func&& func) noexcept {
        using trait = function_traits<Func>;
        static_assert(trait::arity == 1, "func can take only one parameter");
        using ex_type = typename trait::template arg<0>::type;
        using func_ret = typename trait::return_type;
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T...> {
            try {
                return make_ready_future<T...>(fut.get());
            } catch(ex_type& ex) {
                return futurize<func_ret>::apply(func, ex);
            }
        });
    }

    /// \brief Ignore any result hold by this future
    ///
    /// Ignore any result (value or exception) hold by this future.
    /// Use with caution since usually ignoring exception is not what
    /// you want
    void ignore_ready_future() noexcept {
        _state.ignore();
    }

#if SEASTAR_COROUTINES_TS
    void set_coroutine(task& coroutine) noexcept {
        assert(!state()->available());
        assert(_promise);
        detach_promise()->set_coroutine(_local_state, coroutine);
    }
#endif
private:
    void set_callback(std::unique_ptr<continuation_base<T...>> callback) {
        if (_state.available()) {
            callback->set_state(get_available_state());
            ::seastar::schedule(std::move(callback));
        } else {
            assert(_promise);
            detach_promise()->schedule(std::move(callback));
        }

    }

    /// \cond internal
    template <typename... U>
    friend class promise;
    template <typename... U, typename... A>
    friend future<U...> make_ready_future(A&&... value);
    template <typename... U>
    friend future<U...> make_exception_future(std::exception_ptr ex) noexcept;
    template <typename... U, typename Exception>
    friend future<U...> make_exception_future(Exception&& ex) noexcept;
    template <typename... U, typename V>
    friend void internal::set_callback(future<U...>&, std::unique_ptr<V>);
    /// \endcond
};

template <typename... T>
inline
future<T...>
promise<T...>::get_future() noexcept {
    assert(!this->_future && this->_state && !this->_task);
    return future<T...>(this);
}

template <typename... T>
template<typename promise<T...>::urgent Urgent>
inline
void promise<T...>::make_ready() noexcept {
    if (_task) {
        _state = nullptr;
        if (Urgent == urgent::yes && !need_preempt()) {
            ::seastar::schedule_urgent(std::move(_task));
        } else {
            ::seastar::schedule(std::move(_task));
        }
    }
}

template <typename... T>
inline
promise<T...>::promise(promise&& x) noexcept : promise_base(std::move(x)) {
    if (this->_state == &x._local_state) {
        this->_state = &_local_state;
        _local_state = std::move(x._local_state);
    }
}

template <typename... T, typename... A>
inline
future<T...> make_ready_future(A&&... value) {
    return future<T...>(ready_future_marker(), std::forward<A>(value)...);
}

template <typename... T>
inline
future<T...> make_exception_future(std::exception_ptr ex) noexcept {
    return future<T...>(exception_future_marker(), std::move(ex));
}

void log_exception_trace() noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This no I/O needs to be performed to perform a computation
/// (for example, because the connection is closed and we cannot read
/// from it).
template <typename... T, typename Exception>
inline
future<T...> make_exception_future(Exception&& ex) noexcept {
    log_exception_trace();
    return make_exception_future<T...>(std::make_exception_ptr(std::forward<Exception>(ex)));
}

/// @}

/// \cond internal

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
    try {
        return convert(::seastar::apply(std::forward<Func>(func), std::move(args)));
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::apply(Func&& func, FuncArgs&&... args) noexcept {
    try {
        return convert(func(std::forward<FuncArgs>(args)...));
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

template <typename Ret>  // Ret = void | future<>
struct do_void_futurize_helper;

template <>
struct do_void_futurize_helper<void> {
    template <typename Func, typename... FuncArgs>
    static future<> apply(Func&& func, FuncArgs&&... args) noexcept {
        try {
            func(std::forward<FuncArgs>(args)...);
            return make_ready_future<>();
        } catch (...) {
            return make_exception_future(std::current_exception());
        }
    }

    template<typename Func, typename... FuncArgs>
    static future<> apply_tuple(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
        try {
            ::seastar::apply(std::forward<Func>(func), std::move(args));
            return make_ready_future<>();
        } catch (...) {
            return make_exception_future(std::current_exception());
        }
    }
};

template <>
struct do_void_futurize_helper<future<>> {
    template <typename Func, typename... FuncArgs>
    static future<> apply(Func&& func, FuncArgs&&... args) noexcept {
        try {
            return func(std::forward<FuncArgs>(args)...);
        } catch (...) {
            return make_exception_future(std::current_exception());
        }
    }

    template<typename Func, typename... FuncArgs>
    static future<> apply_tuple(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
        try {
            return ::seastar::apply(std::forward<Func>(func), std::move(args));
        } catch (...) {
            return make_exception_future(std::current_exception());
        }
    }
};

template <typename Func, typename... FuncArgs>
using void_futurize_helper = do_void_futurize_helper<std::result_of_t<Func(FuncArgs&&...)>>;

template<typename Func, typename... FuncArgs>
typename futurize<void>::type futurize<void>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
    return void_futurize_helper<Func, FuncArgs...>::apply_tuple(std::forward<Func>(func), std::move(args));
}

template<typename Func, typename... FuncArgs>
typename futurize<void>::type futurize<void>::apply(Func&& func, FuncArgs&&... args) noexcept {
    return void_futurize_helper<Func, FuncArgs...>::apply(std::forward<Func>(func), std::forward<FuncArgs>(args)...);
}

template<typename... Args>
template<typename Func, typename... FuncArgs>
typename futurize<future<Args...>>::type futurize<future<Args...>>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
    try {
        return ::seastar::apply(std::forward<Func>(func), std::move(args));
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

template<typename... Args>
template<typename Func, typename... FuncArgs>
typename futurize<future<Args...>>::type futurize<future<Args...>>::apply(Func&& func, FuncArgs&&... args) noexcept {
    try {
        return func(std::forward<FuncArgs>(args)...);
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

template <typename T>
template <typename Arg>
inline
future<T>
futurize<T>::make_exception_future(Arg&& arg) {
    return ::seastar::make_exception_future<T>(std::forward<Arg>(arg));
}

template <typename... T>
template <typename Arg>
inline
future<T...>
futurize<future<T...>>::make_exception_future(Arg&& arg) {
    return ::seastar::make_exception_future<T...>(std::forward<Arg>(arg));
}

template <typename Arg>
inline
future<>
futurize<void>::make_exception_future(Arg&& arg) {
    return ::seastar::make_exception_future<>(std::forward<Arg>(arg));
}

template <typename T>
inline
future<T>
futurize<T>::from_tuple(std::tuple<T>&& value) {
    return make_ready_future<T>(std::move(value));
}

template <typename T>
inline
future<T>
futurize<T>::from_tuple(const std::tuple<T>& value) {
    return make_ready_future<T>(value);
}

inline
future<>
futurize<void>::from_tuple(std::tuple<>&& value) {
    return make_ready_future<>();
}

inline
future<>
futurize<void>::from_tuple(const std::tuple<>& value) {
    return make_ready_future<>();
}

template <typename... Args>
inline
future<Args...>
futurize<future<Args...>>::from_tuple(std::tuple<Args...>&& value) {
    return make_ready_future<Args...>(std::move(value));
}

template <typename... Args>
inline
future<Args...>
futurize<future<Args...>>::from_tuple(const std::tuple<Args...>& value) {
    return make_ready_future<Args...>(value);
}

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, Args&&... args) {
    using futurator = futurize<std::result_of_t<Func(Args&&...)>>;
    return futurator::apply(std::forward<Func>(func), std::forward<Args>(args)...);
}

namespace internal {

template <typename... T, typename U>
inline
void set_callback(future<T...>& fut, std::unique_ptr<U> callback) {
    // It would be better to use continuation_base<T...> for U, but
    // then a derived class of continuation_base<T...> won't be matched
    return fut.set_callback(std::move(callback));
}

}


/// \endcond

}
