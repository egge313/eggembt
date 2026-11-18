// GLM-5.3-Flash: T-lists (doku section 16).
//
// To each marking, a list of truncated traces -- a T-list -- records the
// recent history from which we arrived to the marking. The length of the list
// is N (default 4, capped at kMaxTList). The Nth element of the T-list is a
// mini-hash created from the current trace. The (marking, T-list) pairs are
// the basis for pheromone generation instead of mere markings (a fine layer
// beside the edge channels; see engine/AntColony.h).
#pragma once

#include "model/Net.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pn {

// The list length is capped: a longer recent history rarely changes where a
// campaign goes, and the pair keys grow with it.
inline constexpr std::size_t kMaxTList = 8;

// The start value of every element: the hash of an empty suffix. Arbitrary
// but fixed, so every platform and every replay produces the same lists.
inline constexpr std::uint32_t kTraceSeed = 0x5EED5EEDu;

// The identity of a trace step: the transition's label if the transition has
// one, else "I:" + transition id for a silent internal step. Uniform across
// path and event runs, and stable under replay.
inline std::string stepIdentity(const Transition &t)
{
    return !t.label.empty() ? t.label : "I:" + t.id;
}

// The mini-hash of one trace step: FNV-1a over the identity's bytes. 32-bit,
// platform-independent, no std::hash anywhere.
inline std::uint32_t stepHash(const std::string &step)
{
    std::uint32_t h = 2166136261u;  // FNV-1a offset basis
    for (const char c : step) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;  // FNV-1a prime
    }
    return h;
}

inline std::uint32_t stepHashOf(const Transition &t)
{
    return stepHash(stepIdentity(t));
}

// Folds one step into a 32-bit carry: xor, then multiply with the golden-ratio
// constant, then a Murmur-style final avalanche. Written out so every
// platform, compiler and replay produces the same values.
inline std::uint32_t mix(std::uint32_t carry, std::uint32_t step)
{
    std::uint32_t h = carry ^ step;
    h *= 0x9E3779B9u;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// A T-list: N mini-hashes, element k (k = 1..N) being the mini-hash of the
// trace's last k steps.
//
// THE SUFFIX CASCADE, EXACTLY. A run's trace is its sequence of steps
// s_1, ..., s_m, each hashed by stepHashOf() of the transition that fired.
// With kTraceSeed as the hash of an empty suffix, element k after m steps is
//
//     H_k(m) = mix(H_{k-1}(m-1), s_m)     for k = 1..N,     H_k(0) = kTraceSeed
//
// so H_1(m) = mix(kTraceSeed, s_m) depends only on the last step, H_2(m) on
// the last two, ..., and H_N(m) is the mini-hash created from the current
// trace, truncated to its last N steps -- the Nth element of the T-list. Two
// arrivals at a marking share the first j elements of their T-lists exactly
// when their traces' last j steps agree.
//
// The recurrence lets one rolling pass per arriving step maintain the whole
// list (h_[i] holds element i+1):
//
//     carry = kTraceSeed
//     for i in 0..len-1:
//         older = h_[i]               // h_[i] held H_{i+1}(m-1)
//         h_[i] = mix(carry, s_m)     // becomes H_{i+1}(m)
//         carry = older               // H_{i+1}(m-1) is H_i(m-1) for the next i
//
// One arriving step updates all N elements in O(N); pricing a candidate edge
// extends the list by the edge's step -- the same pass on a copy, O(N) per
// candidate. At reset (a run's begin) all N elements are kTraceSeed, and a
// trace shorter than an element's length hashes deterministically: with m
// steps taken, element i+1 has been updated min(i+1, m) times and holds the
// hash of the whole trace so far -- the missing deeper history contributes
// kTraceSeed, the hash of nothing.
class TraceCascade {
public:
    TraceCascade() { h_.fill(kTraceSeed); }

    // 0 switches the layer off; longer values are capped at kMaxTList. Set
    // the length before the first push (the session and the colony set it
    // once from the configuration).
    void setLength(std::size_t n) { length_ = n < kMaxTList ? n : kMaxTList; }
    std::size_t length() const { return length_; }

    // A run's begin: all N elements are kTraceSeed.
    void reset() { h_.fill(kTraceSeed); }

    // One arriving step: the rolling pass above, exactly.
    void push(std::uint32_t step)
    {
        std::uint32_t carry = kTraceSeed;
        for (std::size_t i = 0; i < length_; ++i) {
            const std::uint32_t older = h_[i];
            h_[i] = mix(carry, step);
            carry = older;
        }
    }

    // The raw elements: h_[i] is element i+1; entries at or beyond length()
    // stay kTraceSeed.
    const std::array<std::uint32_t, kMaxTList> &elements() const { return h_; }

    // The list extended by one more step, without touching *this -- used to
    // price a candidate edge under the pair its target would create.
    TraceCascade extended(std::uint32_t step) const
    {
        TraceCascade out = *this;
        out.push(step);
        return out;
    }

    bool operator==(const TraceCascade &o) const { return length_ == o.length_ && h_ == o.h_; }
    bool operator!=(const TraceCascade &o) const { return !(*this == o); }

private:
    std::size_t length_ = 0;
    std::array<std::uint32_t, kMaxTList> h_;
};

}  // namespace pn
