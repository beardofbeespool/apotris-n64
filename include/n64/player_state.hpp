#pragma once

// N64 two-player split-screen: swapping a file's per-player globals.
//
// The game keeps its state in globals. With two players, one set is live and
// the other waits in a stash; switching player swaps the two. Each file lists
// its per-player globals and gets a swap function:
//
//     #define MY_STATE(X) X(game) X(clearTimer) X(effectList)
//     N64_PLAYER_STATE(n64SwapMyState, MY_STATE)
//
// The stash starts as a copy of the globals' initial values, so the second
// player starts from the same state the first did. See docs/n64-two-player.md.

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace n64state {

template <typename T> void copy(T& dst, const T& src) { dst = src; }

template <typename T, size_t N> void copy(T (&dst)[N], const T (&src)[N]) {
    for (size_t i = 0; i < N; i++)
        copy(dst[i], src[i]);
}

// Plain arrays (the board state) are swapped with memcpy, much quicker than
// element by element; everything else (lists, strings) with std::swap.
template <typename T> void swap(T& a, T& b) {
    if constexpr (std::is_array_v<T> && std::is_trivially_copyable_v<T>) {
        alignas(8) static unsigned char temp[sizeof(T)];
        memcpy(temp, &a, sizeof(T));
        memcpy(&a, &b, sizeof(T));
        memcpy(&b, temp, sizeof(T));
    } else {
        std::swap(a, b);
    }
}

} // namespace n64state

#define N64_STATE_MEMBER(name) decltype(::name) name;
#define N64_STATE_COPY(name) n64state::copy(name, ::name);
#define N64_STATE_SWAP(name) n64state::swap(::name, stash.name);

// Defines swapFn(), which swaps the globals in LIST with their stash. Use at
// file scope, after the globals' definitions (the stash copies their initial
// values when it is constructed).
#define N64_PLAYER_STATE(swapFn, LIST)                                         \
    namespace {                                                                \
    struct swapFn##_State {                                                    \
        LIST(N64_STATE_MEMBER)                                                 \
        swapFn##_State() { LIST(N64_STATE_COPY) }                              \
    };                                                                         \
    swapFn##_State swapFn##_stash;                                             \
    }                                                                          \
    void swapFn() {                                                            \
        auto& stash = swapFn##_stash;                                          \
        LIST(N64_STATE_SWAP)                                                   \
    }
