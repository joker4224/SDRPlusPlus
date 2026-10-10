// SPDX-License-Identifier: GPL-3.0-only
// Tracking adapted from SDRPlusPlus_SwissKnifeEdition's ADS-B module.
// See ../THIRD_PARTY.md for the source revision and changes.
#pragma once

#include "adsb/adsb.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace adsb {

struct Aircraft {
    uint32_t icao = 0;
    std::string callsign;
    bool havePos = false, haveAlt = false, haveSpeed = false;
    bool haveHeading = false, haveVrate = false;
    bool airspeed = false, trueAirspeed = false, verticalRateBarometric = false;
    bool surface = false;
    double lat = 0, lon = 0, speedKt = 0, headingDeg = 0;
    int altFt = 0, vrateFpm = 0;
    double lastSeen = 0;
    uint64_t messages = 0;
};

// Caller owns synchronization. All times are monotonic seconds. A snapshot
// expires individual fields even while other messages keep an aircraft alive.
class AircraftTracker {
public:
    static constexpr double FIELD_TTL = 15.0;
    static constexpr double CALLSIGN_TTL = 300.0;
    static constexpr double AIRCRAFT_TTL = 60.0;
    static constexpr double CPR_PAIR_TTL = 10.0;

    explicit AircraftTracker(std::size_t capacity = 512) : capacity((std::max)(std::size_t(1), capacity)) {}

    void update(const Message& m, double now, double signalTime = std::numeric_limits<double>::quiet_NaN()) {
        if (m.kind == KIND_UNKNOWN || !std::isfinite(now) || m.icao == 0 || m.icao > 0xFFFFFF) { return; }
        if (!std::isfinite(signalTime)) { signalTime = now; }
        expire(now);
        if (records.find(m.icao) == records.end() && records.size() >= capacity) {
            auto oldest = std::min_element(records.begin(), records.end(), [](const auto& a, const auto& b) {
                return a.second.aircraft.lastSeen < b.second.aircraft.lastSeen;
            });
            records.erase(oldest);
        }
        auto& r = records[m.icao];
        auto& ac = r.aircraft;
        ac.icao = m.icao;
        ac.lastSeen = now;
        ++ac.messages;

        if (m.kind == KIND_IDENT) {
            ac.callsign = m.callsign;
            r.callsignTime = now;
        }
        else if (m.kind == KIND_SURFACE_POS) {
            // A surface frame invalidates airborne data. Surface CPR needs a
            // nearby reference, which this basic receiver deliberately omits.
            clearMotion(r);
            ac.surface = true;
        }
        else if (m.kind == KIND_VELOCITY) {
            if (ac.surface) { clearMotion(r); }
            ac.surface = false;
            ac.haveSpeed = m.hasGroundSpeed;
            ac.speedKt = m.speedKt;
            ac.airspeed = m.airspeed;
            ac.trueAirspeed = m.trueAirspeed;
            ac.haveHeading = m.hasHeading;
            ac.headingDeg = m.headingDeg;
            ac.haveVrate = m.hasVerticalRate;
            ac.vrateFpm = m.verticalRateFpm;
            ac.verticalRateBarometric = m.verticalRateBarometric;
            r.velocityTime = now;
        }
        else if (m.kind == KIND_AIRBORNE_POS) {
            if (ac.surface) { clearMotion(r); }
            ac.surface = false;
            // Barometric (9-18) and GNSS (20-22) position messages must not
            // share a CPR pair. GNSS altitude is not decoded by this port.
            int family = m.typeCode >= 20 ? 2 : 1;
            if (r.positionFamily != family) {
                r.even.valid = r.odd.valid = false;
                ac.havePos = false;
                r.positionFamily = family;
            }
            ac.haveAlt = m.hasAltitude;
            ac.altFt = m.altitudeFt;
            r.altitudeTime = now;
            auto& half = m.cprOddFrame ? r.odd : r.even;
            half = {true, m.cprLat, m.cprLon, now, signalTime};
            if (r.even.valid && r.odd.valid &&
                std::fabs(r.even.received - r.odd.received) <= CPR_PAIR_TTL &&
                std::fabs(r.even.signalTime - r.odd.signalTime) <= CPR_PAIR_TTL) {
                double lat = 0, lon = 0;
                if (cprDecodeGlobal(r.even.lat, r.even.lon, r.odd.lat, r.odd.lon,
                                    m.cprOddFrame, false, lat, lon)) {
                    ac.havePos = true;
                    ac.lat = lat;
                    ac.lon = lon;
                    r.positionTime = now;
                }
            }
        }
    }

    std::vector<Aircraft> snapshot(double now) {
        expire(now);
        std::vector<Aircraft> result;
        result.reserve(records.size());
        for (const auto& item : records) { result.push_back(item.second.aircraft); }
        return result;
    }

    void clear() { records.clear(); }

private:
    struct CprHalf {
        bool valid = false;
        uint32_t lat = 0, lon = 0;
        double received = 0, signalTime = 0;
    };
    struct Record {
        Aircraft aircraft;
        CprHalf even, odd;
        int positionFamily = 0;
        double callsignTime = 0, positionTime = 0, altitudeTime = 0, velocityTime = 0;
    };

    static void clearMotion(Record& r) {
        auto& ac = r.aircraft;
        ac.havePos = ac.haveAlt = ac.haveSpeed = ac.haveHeading = ac.haveVrate = false;
        r.even.valid = r.odd.valid = false;
        r.positionFamily = 0;
    }

    void expire(double now) {
        for (auto it = records.begin(); it != records.end();) {
            auto& r = it->second;
            auto& ac = r.aircraft;
            if (now < ac.lastSeen || now - ac.lastSeen > AIRCRAFT_TTL) {
                it = records.erase(it);
                continue;
            }
            if (now - r.callsignTime > CALLSIGN_TTL) { ac.callsign.clear(); }
            if (now - r.positionTime > FIELD_TTL) { ac.havePos = false; }
            if (now - r.altitudeTime > FIELD_TTL) { ac.haveAlt = false; }
            if (now - r.velocityTime > FIELD_TTL) {
                ac.haveSpeed = ac.haveHeading = ac.haveVrate = false;
            }
            if (now - r.even.received > CPR_PAIR_TTL) { r.even.valid = false; }
            if (now - r.odd.received > CPR_PAIR_TTL) { r.odd.valid = false; }
            ++it;
        }
    }

    std::size_t capacity;
    std::map<uint32_t, Record> records;
};

} // namespace adsb
