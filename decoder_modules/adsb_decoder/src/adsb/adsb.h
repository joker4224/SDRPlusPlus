// Adapted from F4JTV/SDRPlusPlus_SwissKnifeEdition, GPL-3.0.
// Source revision: bb6351b98b58b724949c017e42d312030fbbd3f3.
#pragma once
#include <stdint.h>
#include <string>
#include <cstddef>

// ----------------------------------------------------------------------------
// Pure ADS-B (Mode S Extended Squitter, 1090 MHz) decoding logic.
//
// This file has NO dependency on SDR++ so it can be unit-tested standalone.
// It operates on raw decoded frames (already demodulated/sliced into bytes).
//
//  - Mode S CRC-24 verification (DF17 syndrome must be 0).
//  - Downlink Format / ICAO address extraction.
//  - DF17 ADS-B message parsing:
//      * Aircraft identification (TC 1-4)  -> callsign + category
//      * Airborne position    (TC 9-18,20-22) -> CPR halves; Q=1 barometric altitude
//      * Surface position     (TC 5-8)        -> transition marker only
//      * Airborne velocity    (TC 19)         -> speed, heading, vertical rate
//  - Airborne CPR global decode (fresh even+odd pair supplied by the tracker).
// ----------------------------------------------------------------------------

namespace adsb {

    // A raw Mode S frame: 7 bytes (short, 56 bits) or 14 bytes (long, 112 bits).
    struct RawFrame {
        uint8_t bytes[14]{};
        int len = 0; // 7 or 14
        uint64_t sampleOffset = 0; // preamble offset since detector reset
    };

    // Compute the Mode S 24-bit CRC syndrome over a frame.
    // For a valid DF17/DF18 frame (parity overlaid with 0), this returns 0.
    uint32_t crc(const uint8_t* msg, int lenBytes);

    // Downlink Format (top 5 bits).
    inline int downlinkFormat(const uint8_t* msg) { return (msg[0] >> 3) & 0x1F; }

    // ICAO 24-bit address. For DF17/18 it is bits 9..32 (bytes 1..3).
    inline uint32_t icaoAddress(const uint8_t* msg) {
        return ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];
    }

    // ADS-B Type Code (first 5 bits of the ME field; ME starts at byte 4).
    inline int typeCode(const uint8_t* msg) { return (msg[4] >> 3) & 0x1F; }

    // Decoded message kinds.
    enum MsgKind {
        KIND_UNKNOWN = 0,
        KIND_IDENT,            // callsign
        KIND_AIRBORNE_POS,     // lat/lon (CPR) + altitude
        KIND_SURFACE_POS,      // transition marker; no surface position decode
        KIND_VELOCITY          // speed/heading/vertical rate
    };

    // Result of decoding a single CRC-valid DF17 ADS-B message.
    struct Message {
        MsgKind kind = KIND_UNKNOWN;
        uint32_t icao = 0;
        int typeCode = 0;

        // Identification
        std::string callsign;      // trimmed
        int category = 0;

        // Position (raw CPR halves; need a fresh global pair to get lat/lon)
        bool cprOddFrame = false;  // F bit: false=even, true=odd
        uint32_t cprLat = 0;       // 17-bit
        uint32_t cprLon = 0;       // 17-bit
        bool hasAltitude = false;
        int altitudeFt = 0;        // barometric, feet

        // Velocity (TC 19)
        bool hasGroundSpeed = false; // historical name: also true for airspeed
        double speedKt = 0.0;      // knots (ground speed or airspeed)
        bool airspeed = false;     // true if IAS/TAS instead of ground speed
        bool trueAirspeed = false; // meaningful when airspeed: TAS rather than IAS
        bool hasHeading = false;
        double headingDeg = 0.0;
        bool hasVerticalRate = false;
        bool verticalRateBarometric = false; // otherwise GNSS/geometric
        int verticalRateFpm = 0;   // feet per minute, signed
    };

    // Parse a DF17 long frame (14 bytes) into a Message. The caller must supply
    // the actual available length. Always clears out; rejects invalid CRC/DF.
    // Returns true if the message kind was recognised.
    bool parseAdsb(const uint8_t* msg, Message& out, int lenBytes = 14);

    // CPR: number of longitude zones for a given latitude (NL function).
    int cprNL(double lat);

    // Global CPR decode from an even and an odd frame.
    //  cprLatE/cprLonE : 17-bit CPR halves from the even frame
    //  cprLatO/cprLonO : 17-bit CPR halves from the odd frame
    //  mostRecentOdd   : true if the odd frame is the most recent of the pair
    //  surface         : retained for compatibility; true is rejected
    // The caller must pair frames from the same aircraft and altitude source,
    // received no more than 10 seconds apart. This pure function has no clock.
    // Returns true on success and fills lat/lon (degrees). On failure leaves
    // both outputs unchanged. Surface CPR needs a reference and is unsupported.
    bool cprDecodeGlobal(uint32_t cprLatE, uint32_t cprLonE,
                         uint32_t cprLatO, uint32_t cprLonO,
                         bool mostRecentOdd, bool surface,
                         double& lat, double& lon);

} // namespace adsb
