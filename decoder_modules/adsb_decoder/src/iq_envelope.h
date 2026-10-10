// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>

namespace adsb {
// Shared by the native IQ sink and the standalone reception tests.
inline float iqEnvelope(float real, float imag) {
    return std::hypot(real, imag);
}
}
