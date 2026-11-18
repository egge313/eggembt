// GLM-5.3-Flash: the credibility experiment behind the crash predictions.
//
// Egge: "Sometimes an SUT crash is nondeterministic. It can be random, rare,
// sometimes, or often (but not always). Any of these indicate a bug. Therefore
// an SUT crash should be treated as a target until we are certain that it is
// deterministic." -- and, on the certainty rule: "Credibility level on a
// binomial distribution."
//
// The experiment. Every visit of the predicted place while the prediction is
// unresolved is one attempt: a definite step of the predicted transition, or a
// run that is certainly in the predicted marking. The first attributed crash
// opens the experiment and counts as the first attempt; every later visit is
// another attempt, ending in a crash (a hit: the run ends sut_crashed and is
// attributed to this prediction) or not (a miss). With k crashes in n
// attempts, the crash probability at the place has the posterior
//
//     Beta(a0 + k, b0 + n - k),   prior Beta(a0, b0) = Beta(k0*p0, k0*(1-p0))
//
// with k0 = 2 pseudo-observations and p0 the prior mean that the probable
// class word (or the explicit "p") carries.
//
// CERTAIN DETERMINISTIC. k = n is the logic of it (a deterministic crash has
// no misses); the credibility level turns it into a statement about the
// future: with credibility gamma (the --crash-level option, 0.95) the crash
// probability is at least theta = 0.90,
//
//     k = n  and  I_theta(a0 + n, b0) <= 1 - gamma,
//
// where I_x(a, b) is the regularized incomplete beta function, the CDF of the
// Beta distribution. Then the crash is as good as deterministic and the
// campaign treats the way in to the place like any other crash: a catastrophe.
//
// EXHAUSTED. n reaches the give-up budget of the class (or of the explicit
// "attempts") without certainty. With at least one crash (k >= 1) the bug is
// real: the campaign treats the way in like any other crash. Without any
// crash (k = 0) the prediction never materialised: the campaign stops seeking
// and teaches nothing.
//
// SEEKING. Anything else, including n = 0 (nothing has happened at the place
// yet): the crash is still a target.
#pragma once

#include "model/Net.h"  // CrashClass

#include <cmath>
#include <cstdint>

namespace pn {

// The prior weight: k0 pseudo-observations behind every class word. Two is the
// smallest number that lets a class mean something without shouting.
inline constexpr double kCrashPriorWeight = 2.0;

// The determinism threshold theta: "certain deterministic" means the crash
// probability is at least this much, with the credibility level.
inline constexpr double kCrashTheta = 0.90;

namespace detail {

// The continued fraction of the incomplete beta ratio, by Lentz's modified
// method (as in Numerical Recipes' betacf). Converges for 0 < x < 1 whenever
// the caller picked the sensible branch (see regularizedIncompleteBeta).
inline double betaContinuedFraction(double a, double b, double x)
{
    constexpr int kMaxIterations = 300;
    constexpr double kEpsilon = 1e-14;  // relative convergence
    constexpr double kTiny = 1e-300;    // guard against division by zero

    const double qab = a + b;
    const double qam = a - 1.0;
    const double qap = a + 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::fabs(d) < kTiny) d = kTiny;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= kMaxIterations; ++m) {
        const int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < kEpsilon) break;
    }
    return h;
}

}  // namespace detail

// The regularized incomplete beta function I_x(a, b) = P(Beta(a, b) <= x),
// the CDF of the Beta distribution, for a, b > 0 and 0 <= x <= 1.
inline double regularizedIncompleteBeta(double a, double b, double x)
{
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double front = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b)
                                + a * std::log(x) + b * std::log1p(-x));
    if (x < (a + 1.0) / (a + b + 2.0))
        return front * detail::betaContinuedFraction(a, b, x) / a;
    return 1.0 - front * detail::betaContinuedFraction(b, a, 1.0 - x) / b;
}

// The calibration a probable class (or an implicit crash) starts from: the
// prior mean p0 of the crash probability and the give-up budget of the
// reproduction experiment.
struct CrashCalibration {
    double priorP = 0.5;
    std::uint32_t giveUpAfter = 6;  // the default of --crash-attempts
};

// The class table: a stronger claim needs fewer reproductions. An implicit
// crash (nobody predicted it) uses the neutral prior and the --crash-attempts
// budget instead of this table.
inline CrashCalibration classCalibration(CrashClass c)
{
    switch (c) {
    case CrashClass::Always:    return {0.98, 2};
    case CrashClass::Often:     return {0.80, 4};
    case CrashClass::Sometimes: return {0.50, 8};
    case CrashClass::Rare:      return {0.10, 20};
    case CrashClass::Random:    return {0.50, 20};
    }
    return {0.50, 20};
}

// The attempts and crashes counted for one prediction so far.
struct CrashStats {
    std::uint64_t attempts = 0;  // visits of the place while unresolved
    std::uint64_t crashes = 0;   // of them, runs that died there attributed to it
};

// Where a prediction stands in its experiment.
enum class CrashStatus {
    Seeking,       // the crash is still a target (also before the first visit)
    Deterministic, // certain: treat the way in like any other crash
    Exhausted,     // gave up with k >= 1: the bug is real, treat it like a crash
    Unobserved     // gave up with k = 0: the crash never showed up, teach nothing
};

inline const char *toString(CrashStatus s)
{
    switch (s) {
    case CrashStatus::Seeking:       return "seeking";
    case CrashStatus::Deterministic: return "deterministic";
    case CrashStatus::Exhausted:     return "exhausted";
    case CrashStatus::Unobserved:    return "unobserved";
    }
    return "seeking";
}

// The rule above, in one function. gamma is the credibility level
// (--crash-level). Statuses only ever advance: attempts stop being counted
// once the prediction is resolved.
inline CrashStatus crashStatus(const CrashStats &stats, const CrashCalibration &cal,
                               double gamma)
{
    const double a0 = kCrashPriorWeight * cal.priorP;
    const double b0 = kCrashPriorWeight * (1.0 - cal.priorP);
    const double a = a0 + static_cast<double>(stats.crashes);
    const double b = b0 + static_cast<double>(stats.attempts - stats.crashes);
    if (stats.crashes >= 1 && stats.crashes == stats.attempts
        && regularizedIncompleteBeta(a, b, kCrashTheta) <= 1.0 - gamma)
        return CrashStatus::Deterministic;
    if (stats.attempts >= cal.giveUpAfter)
        return stats.crashes >= 1 ? CrashStatus::Exhausted : CrashStatus::Unobserved;
    return CrashStatus::Seeking;
}

}  // namespace pn
