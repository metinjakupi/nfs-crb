// A bounded high-water mark for a queried native presentation timeline.
// Never extrapolates: small estimate corrections hold video until the device
// catches up. Explicit device discontinuities are handled by the caller.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mf {
class PresentationClock {
  public:
    bool observe(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0)
            return false;
        const double regression = high_water_ - seconds;
        // Direct silence-only AudioQueue traces showed 3.6ms corrections with
        // no discontinuity flag. Bound the hold to10ms (below one30fps frame),
        // retain measurements, and reject larger jumps instead of inventing time.
        if (regression > .010)
            return false;
        if (regression > 0) {
            ++corrections_;
            maximum_ = std::max(maximum_, regression);
        }
        high_water_ = std::max(high_water_, seconds);
        return true;
    }
    double seconds() const {
        return high_water_;
    }
    double max_correction() const {
        return maximum_;
    }
    uint64_t corrections() const {
        return corrections_;
    }

  private:
    double high_water_ = 0, maximum_ = 0;
    uint64_t corrections_ = 0;
};
} // namespace mf
