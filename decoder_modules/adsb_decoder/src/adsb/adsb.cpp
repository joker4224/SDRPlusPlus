// Adapted from F4JTV/SDRPlusPlus_SwissKnifeEdition, GPL-3.0.
// Source revision: bb6351b98b58b724949c017e42d312030fbbd3f3.
#include "adsb.h"
#include <cmath>
#include <cstring>

namespace adsb {

    static constexpr double PI = 3.14159265358979323846;

    // Mode S CRC-24 via direct polynomial long division.
    // Generator: x^24 + x^23 + x^22 + x^21 + x^20 + x^19 + x^18 + x^17 + x^16
    //          + x^15 + x^14 + x^13 + x^12 + x^10 + x^3 + 1  =  0x1FFF409 (25 bits).
    // The 24 parity bits of a valid DF17/DF18 frame make the whole 112-bit frame
    // divisible by G(x), so the remainder (syndrome) is 0 for a clean frame.
    uint32_t crc(const uint8_t* msg, int lenBytes) {
        if (!msg || (lenBytes != 7 && lenBytes != 14)) return 0xFFFFFFu;
        const uint32_t G = 0x1FFF409u; // 25-bit generator
        int bits = lenBytes * 8;
        uint32_t rem = 0;
        for (int i = 0; i < bits; i++) {
            int byte = i >> 3;
            int bit = i & 7;
            uint32_t inbit = (msg[byte] >> (7 - bit)) & 1u;
            rem = (rem << 1) | inbit;
            if (rem & 0x1000000u) { rem ^= G; } // bit 24 set -> subtract generator
        }
        return rem & 0x00FFFFFFu;
    }

    // Extract `len` bits starting at 1-based bit position `start` within the
    // ME field (which begins at byte index 4 of the message).
    static uint32_t meBits(const uint8_t* msg, int start, int len) {
        // The ME field is 56 bits, message bits 33..88 (1-based on full frame).
        // Convert ME-relative (1-based) to full-frame bit index (0-based).
        int firstBit = 32 + (start - 1); // 0-based on full frame
        uint32_t v = 0;
        for (int i = 0; i < len; i++) {
            int b = firstBit + i;
            int byte = b >> 3;
            int bit = b & 7;
            v = (v << 1) | ((msg[byte] >> (7 - bit)) & 1);
        }
        return v;
    }

    static std::string decodeCallsign(const uint8_t* msg) {
        // 8 characters, 6 bits each, ME bits 9..56.
        static const char* charset =
            "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
        std::string cs;
        for (int i = 0; i < 8; i++) {
            int code = meBits(msg, 9 + i * 6, 6);
            char c = charset[code & 0x3F];
            cs += c;
        }
        // Trim trailing spaces and stray '?' padding.
        size_t end = cs.find_last_not_of(" ?");
        if (end == std::string::npos) return "";
        cs.erase(end + 1);
        // Drop leading spaces if any.
        size_t begin = cs.find_first_not_of(" ");
        if (begin != std::string::npos && begin > 0) cs.erase(0, begin);
        return cs;
    }

    // 12-bit barometric altitude (airborne position). Returns feet, hasAlt flag.
    static bool decodeAltitude(const uint8_t* msg, int& altFt) {
        uint32_t alt12 = meBits(msg, 9, 12); // ME bits 9..20
        if (alt12 == 0) return false;
        bool qBit = (alt12 >> 4) & 1; // Q-bit is bit 8 of the 12-bit field
        if (qBit) {
            // 25-foot increments. Remove the Q bit (bit 4), concatenate.
            int n = ((alt12 & 0xFE0) >> 1) | (alt12 & 0x0F);
            altFt = n * 25 - 1000;
            return true;
        }
        // Gillham (Gray) coded altitude (Q=0) is rare on ADS-B; skip for now.
        return false;
    }

    static bool decodeVelocity(const uint8_t* msg, Message& out) {
        int subtype = meBits(msg, 6, 3);
        if (subtype == 1 || subtype == 2) {
            // Ground speed (subtype 1 subsonic, 2 supersonic).
            int dew = meBits(msg, 14, 1);
            int vew = meBits(msg, 15, 10);
            int dns = meBits(msg, 25, 1);
            int vns = meBits(msg, 26, 10);
            if (vew == 0 || vns == 0) {
                // Velocity not available.
            } else {
                double mult = (subtype == 2) ? 4.0 : 1.0;
                double vx = (vew - 1) * mult * (dew ? -1.0 : 1.0); // east-west (east +)
                double vy = (vns - 1) * mult * (dns ? -1.0 : 1.0); // north-south (north +)
                out.speedKt = std::sqrt(vx * vx + vy * vy);
                out.hasGroundSpeed = true;
                out.airspeed = false;
                double hdg = std::atan2(vx, vy) * 180.0 / PI;
                if (hdg < 0) hdg += 360.0;
                out.headingDeg = hdg;
                out.hasHeading = out.speedKt > 0.0;
            }
        }
        else if (subtype == 3 || subtype == 4) {
            // Airspeed + heading.
            int sh = meBits(msg, 14, 1);
            int hdgRaw = meBits(msg, 15, 10);
            if (sh) {
                out.headingDeg = hdgRaw * 360.0 / 1024.0;
                out.hasHeading = true;
            }
            int as = meBits(msg, 26, 10);
            if (as != 0) {
                double mult = (subtype == 4) ? 4.0 : 1.0;
                out.speedKt = (as - 1) * mult;
                out.hasGroundSpeed = true; // we use the same field for display
                out.airspeed = true;
                out.trueAirspeed = meBits(msg, 25, 1) != 0;
            }
        }
        else {
            return false;
        }

        // Vertical rate (common to subtypes 1-4): sign bit 37, value bits 38..46.
        int svr = meBits(msg, 37, 1);
        int vr = meBits(msg, 38, 9);
        if (vr != 0) {
            int rate = (vr - 1) * 64;
            out.verticalRateFpm = svr ? -rate : rate;
            out.hasVerticalRate = true;
            out.verticalRateBarometric = meBits(msg, 36, 1) != 0;
        }
        return true;
    }

    bool parseAdsb(const uint8_t* msg, Message& out, int lenBytes) {
        out = Message();
        if (!msg || lenBytes != 14 || crc(msg, lenBytes) != 0) return false;
        int df = downlinkFormat(msg);
        // DF18 can use a non-ICAO address namespace and rebroadcast position.
        // Do not merge it into direct DF17 aircraft state in this basic decoder.
        if (df != 17) return false;
        out.icao = icaoAddress(msg);
        int tc = typeCode(msg);
        out.typeCode = tc;

        if (tc >= 1 && tc <= 4) {
            out.kind = KIND_IDENT;
            out.category = meBits(msg, 6, 3);
            out.callsign = decodeCallsign(msg);
            return true;
        }
        else if (tc >= 9 && tc <= 18) {
            // Airborne position (barometric altitude).
            out.kind = KIND_AIRBORNE_POS;
            out.cprOddFrame = meBits(msg, 22, 1) != 0; // F bit (ME bit 22)
            out.cprLat = meBits(msg, 23, 17);
            out.cprLon = meBits(msg, 40, 17);
            out.hasAltitude = decodeAltitude(msg, out.altitudeFt);
            return true;
        }
        else if (tc >= 20 && tc <= 22) {
            // Airborne position (GNSS altitude).
            out.kind = KIND_AIRBORNE_POS;
            out.cprOddFrame = meBits(msg, 22, 1) != 0;
            out.cprLat = meBits(msg, 23, 17);
            out.cprLon = meBits(msg, 40, 17);
            return true;
        }
        else if (tc >= 5 && tc <= 8) {
            // A transition marker lets the tracker invalidate airborne data.
            // Surface CPR has unresolved latitude/longitude ambiguity without
            // a nearby reference position, and is deliberately not decoded.
            out.kind = KIND_SURFACE_POS;
            return true;
        }
        else if (tc == 19) {
            out.kind = KIND_VELOCITY;
            if (decodeVelocity(msg, out)) return true;
        }
        out = Message();
        return false;
    }

    int cprNL(double lat) {
        if (!std::isfinite(lat) || lat < -90.0 || lat > 90.0) return 0;
        if (lat == 0.0) return 59;
        double a = std::fabs(lat);
        if (a >= 87.0) {
            if (a > 87.0) return 1;
            return 2;
        }
        const double NZ = 15.0;
        double num = 2.0 * PI;
        double cosLat = std::cos(PI / 180.0 * a);
        double tmp = 1.0 - (1.0 - std::cos(PI / (2.0 * NZ))) / (cosLat * cosLat);
        if (tmp < -1.0) tmp = -1.0;
        if (tmp > 1.0) tmp = 1.0;
        double den = std::acos(tmp);
        return (int)std::floor(num / den);
    }

    static double cprMod(double a, double b) {
        double r = std::fmod(a, b);
        if (r < 0) r += b;
        return r;
    }

    bool cprDecodeGlobal(uint32_t cprLatE, uint32_t cprLonE,
                         uint32_t cprLatO, uint32_t cprLonO,
                         bool mostRecentOdd, bool surface,
                         double& outLat, double& outLon) {
        if (surface || cprLatE > 0x1FFFFu || cprLonE > 0x1FFFFu ||
            cprLatO > 0x1FFFFu || cprLonO > 0x1FFFFu) return false;
        const double scale = 131072.0; // 2^17
        double latE = (double)cprLatE / scale;
        double lonE = (double)cprLonE / scale;
        double latO = (double)cprLatO / scale;
        double lonO = (double)cprLonO / scale;

        const double angle = 360.0;
        double dLatE = angle / 60.0;
        double dLatO = angle / 59.0;

        int j = (int)std::floor(59.0 * latE - 60.0 * latO + 0.5);

        double rlatE = dLatE * (cprMod(j, 60) + latE);
        double rlatO = dLatO * (cprMod(j, 59) + latO);

        // Southern hemisphere adjustment for airborne (values 270..360 -> negative).
        if (rlatE >= 270.0) rlatE -= 360.0;
        if (rlatO >= 270.0) rlatO -= 360.0;
        if (std::fabs(rlatE) > 90.0 || std::fabs(rlatO) > 90.0) return false;

        int nlE = cprNL(rlatE);
        int nlO = cprNL(rlatO);
        if (nlE != nlO) return false; // latitudes in different zones, can't combine

        double lat, lon;
        if (mostRecentOdd) {
            lat = rlatO;
            int nl = cprNL(rlatO);
            int ni = nl - 1; if (ni < 1) ni = 1;
            double dLon = angle / (double)ni;
            int m = (int)std::floor(lonE * (nl - 1) - lonO * nl + 0.5);
            lon = dLon * (cprMod(m, ni) + lonO);
        } else {
            lat = rlatE;
            int nl = cprNL(rlatE);
            int ni = nl; if (ni < 1) ni = 1;
            double dLon = angle / (double)ni;
            int m = (int)std::floor(lonE * (nl - 1) - lonO * nl + 0.5);
            lon = dLon * (cprMod(m, ni) + lonE);
        }
        if (lon >= 180.0) lon -= 360.0;
        if (!std::isfinite(lat) || !std::isfinite(lon) ||
            std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) return false;
        outLat = lat;
        outLon = lon;
        return true;
    }

} // namespace adsb
