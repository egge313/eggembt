// GLM-5.3-Flash: the credibility experiment behind the crash predictions
// (engine/CrashCredibility.h) -- the frozen rule and the closed forms it is
// pinned against.
//
//   prior    Beta(k0*p0, k0*(1-p0)), k0 = 2, p0 from the class word (or the
//            explicit "p")
//   certain  k = n and I_theta(a0 + n, b0) <= 1 - gamma, theta = 0.90
//   budgets  always 2, often 4, sometimes 8, rare 20, random 20; implicit 6
#include "engine/CrashCredibility.h"

#include "MiniTest.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace pn;

namespace {

bool same(double a, double b, double eps = 1e-12)
{
    return std::fabs(a - b) <= eps;
}

}  // namespace

struct TstCrash {
    // The regularized incomplete beta function against its closed forms.
    void closedForms()
    {
        // I_x(a, 1) = x^a
        T_VERIFY(same(regularizedIncompleteBeta(2.5, 1.0, 0.9), std::pow(0.9, 2.5)));
        // I_x(1, b) = 1 - (1-x)^b
        T_VERIFY(same(regularizedIncompleteBeta(1.0, 3.5, 0.4), 1.0 - std::pow(0.6, 3.5)));
        // the endpoints
        T_VERIFY(same(regularizedIncompleteBeta(2.0, 3.0, 0.0), 0.0));
        T_VERIFY(same(regularizedIncompleteBeta(2.0, 3.0, 1.0), 1.0));
        // the integer sum, exact: I_0.5(4, 7) = (sum of C(10, j), j = 4..10) / 2^10
        // = (210 + 252 + 210 + 120 + 45 + 10 + 1) / 1024 = 848 / 1024
        T_VERIFY(same(regularizedIncompleteBeta(4.0, 7.0, 0.5), 848.0 / 1024.0));
        // and the mirrored tail, by the symmetry of x -> 1-x
        T_VERIFY(same(regularizedIncompleteBeta(7.0, 4.0, 0.5), 1.0 - 848.0 / 1024.0));
    }

    // The class table: prior mean and give-up budget of every probable class.
    void classTable()
    {
        const struct Row {
            CrashClass c;
            double p;
            std::uint32_t budget;
        } rows[] = {
            {CrashClass::Always, 0.98, 2},    {CrashClass::Often, 0.80, 4},
            {CrashClass::Sometimes, 0.50, 8}, {CrashClass::Rare, 0.10, 20},
            {CrashClass::Random, 0.50, 20},
        };
        for (const Row &r : rows) {
            const CrashCalibration cal = classCalibration(r.c);
            T_VERIFY(same(cal.priorP, r.p));
            T_COMPARE(cal.giveUpAfter, r.budget);
        }
    }

    // The status rule, pinned per class: the strong prior of "always" makes
    // the first hit certain; the weaker classes ride to their budgets and
    // resolve as exhausted (k >= 1) or unobserved (k = 0) there -- and one
    // step before the budget they still seek.
    void pinnedStatuses()
    {
        const double gamma = 0.95;
        T_COMPARE(toString(crashStatus(CrashStats{1, 1}, classCalibration(CrashClass::Always), gamma)),
                  std::string("deterministic"));
        // nothing has happened yet: the crash is still sought (a target)
        T_COMPARE(toString(crashStatus(CrashStats{}, classCalibration(CrashClass::Rare), gamma)),
                  std::string("seeking"));
        const struct Row {
            CrashClass c;
            std::uint32_t budget;
        } rows[] = {{CrashClass::Often, 4}, {CrashClass::Sometimes, 8}, {CrashClass::Rare, 20},
                    {CrashClass::Random, 20}};
        for (const Row &r : rows) {
            const CrashCalibration cal = classCalibration(r.c);
            T_COMPARE(toString(crashStatus(CrashStats{r.budget - 1, r.budget - 1}, cal, gamma)),
                      std::string("seeking"));
            T_COMPARE(toString(crashStatus(CrashStats{r.budget, r.budget}, cal, gamma)),
                      std::string("exhausted"));
            T_COMPARE(toString(crashStatus(CrashStats{r.budget, 0}, cal, gamma)),
                      std::string("unobserved"));
        }
        // the implicit calibration: the neutral prior, the --crash-attempts budget
        const CrashCalibration implicit{0.5, 6};
        T_COMPARE(toString(crashStatus(CrashStats{6, 6}, implicit, gamma)), std::string("exhausted"));
        T_COMPARE(toString(crashStatus(CrashStats{6, 0}, implicit, gamma)), std::string("unobserved"));
        T_COMPARE(toString(crashStatus(CrashStats{5, 5}, implicit, gamma)), std::string("seeking"));
    }

    // One observed miss falsifies determinism for good (k = n is the logic of
    // it), but straight hits reach it: an explicit p = 0.9 rides a weak prior
    // (a0 = 1.8, b0 = 0.2) and is certain only at the ninth hit -- measured
    // against the closed form: I_0.9(9.8, 0.2) = 0.0531 > 0.05 still seeks,
    // I_0.9(10.8, 0.2) = 0.0453 <= 0.05 is certain.
    void aMissFalsifiesDeterminism()
    {
        const double gamma = 0.95;
        const CrashCalibration cal{0.9, 50};
        T_COMPARE(toString(crashStatus(CrashStats{50, 49}, cal, gamma)), std::string("exhausted"));
        T_COMPARE(toString(crashStatus(CrashStats{9, 9}, cal, gamma)), std::string("deterministic"));
        T_COMPARE(toString(crashStatus(CrashStats{8, 8}, cal, gamma)), std::string("seeking"));
    }

    // For a fixed class, every further straight hit lowers
    // I_theta(a0 + n, b0): certainty only grows.
    void moreHitsAreMoreCertain()
    {
        const CrashCalibration cal = classCalibration(CrashClass::Often);
        double previous = 1.0;
        for (unsigned n = 1; n <= 30; ++n) {
            const double i = regularizedIncompleteBeta(kCrashPriorWeight * cal.priorP + n,
                                                       kCrashPriorWeight * (1.0 - cal.priorP), kCrashTheta);
            T_VERIFY(i < previous);
            previous = i;
        }
    }
};

int main()
{
    T_RUN(TstCrash, closedForms);
    T_RUN(TstCrash, classTable);
    T_RUN(TstCrash, pinnedStatuses);
    T_RUN(TstCrash, aMissFalsifiesDeterminism);
    T_RUN(TstCrash, moreHitsAreMoreCertain);
    return T_SUMMARY();
}
