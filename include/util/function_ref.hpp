#pragma once
// util/function_ref.hpp — Non-owning, trivially-copyable callable reference.
//
// Why not std::function on the match path: std::function type-erases with a
// heap allocation for captures larger than its SBO buffer, is 32 bytes, and
// the call goes through a manager pointer plus an invoker pointer. Cost is
// small in absolute terms (~1–2 ns) but it is the only indirection left on
// the fill path, and it is not needed: the engine owns the sink for the life
// of the book.
//
// function_ref stores two words: an object pointer and a thunk. Calling it is
// one indirect call. It does NOT own the callable — the referenced object
// must outlive the function_ref. Passing a temporary lambda is a bug, and
// the constructor is deleted for rvalue callables to make that a compile
// error rather than a dangling pointer.
//
// Free-function and stateless-lambda callers can use the (ctx, fn) form
// where the context is whatever pointer you want threaded through.

#include <type_traits>
#include <utility>

namespace util {

template<typename Sig> class function_ref;

template<typename R, typename... Args>
class function_ref<R(Args...)> {
public:
    using thunk_t = R (*)(void*, Args...);

    constexpr function_ref() noexcept = default;

    // Bind an lvalue callable. Rvalues are rejected to prevent dangling.
    template<typename F,
             typename = std::enable_if_t<
                 !std::is_same_v<std::decay_t<F>, function_ref> &&
                 std::is_invocable_r_v<R, F&, Args...>>>
    constexpr function_ref(F& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          thunk_([](void* o, Args... a) -> R {
              return (*static_cast<F*>(o))(std::forward<Args>(a)...);
          }) {}

    template<typename F,
             typename = std::enable_if_t<
                 !std::is_same_v<std::decay_t<F>, function_ref> &&
                 std::is_invocable_r_v<R, F&, Args...>>>
    function_ref(F&&) = delete;   // temporary callable would dangle

    // Explicit (context, thunk) form for free functions / C-style sinks.
    constexpr function_ref(void* ctx, thunk_t fn) noexcept : obj_(ctx), thunk_(fn) {}

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return thunk_ != nullptr; }

    R operator()(Args... a) const {
        return thunk_(obj_, std::forward<Args>(a)...);
    }

private:
    void*   obj_   = nullptr;
    thunk_t thunk_ = nullptr;
};

} // namespace util
