// Standalone C++17 tests: no SDR++ runtime, SDR hardware, or third-party libraries.
// Published message examples from The 1090 MHz Riddle by Junzi Sun:
// https://mode-s.org/1090mhz/content/ads-b/2-identification.html
// https://mode-s.org/1090mhz/content/ads-b/3-airborne-position.html
// https://mode-s.org/1090mhz/content/ads-b/5-airborne-velocity.html
#include "adsb/adsb.h"
#include "adsb/frame_detector.h"
#include "aircraft_tracker.h"
#include "iq_envelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Frame = std::array<uint8_t, 14>;
int checks = 0;

void require(bool ok, const std::string& description) {
    ++checks;
    if (!ok) throw std::runtime_error(description);
}

void near(double actual, double expected, double tolerance, const std::string& description) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            description + " (actual " + std::to_string(actual) + ")");
}

Frame fromHex(const std::string& hex) {
    require(hex.size() == 28, "test fixture must contain 112 bits");
    Frame result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return result;
}

// Independent, explicit polynomial division for synthetic fixtures. The decoder's
// crc() is never used to generate parity, avoiding a self-validating CRC test.
void setParity(Frame& frame) {
    frame[11] = frame[12] = frame[13] = 0;
    std::array<unsigned, 112> bits{};
    for (int i = 0; i < 88; ++i) bits[i] = (frame[i / 8] >> (7 - i % 8)) & 1;
    constexpr uint32_t polynomial = 0x1FFF409;
    for (int i = 0; i < 88; ++i) {
        if (!bits[i]) continue;
        for (int j = 0; j <= 24; ++j) bits[i + j] ^= (polynomial >> (24 - j)) & 1;
    }
    for (int i = 88; i < 112; ++i)
        frame[i / 8] |= static_cast<uint8_t>(bits[i] << (7 - i % 8));
}

void setMe(Frame& frame, int first, int width, uint32_t value) {
    for (int i = 0; i < width; ++i) {
        const int bit = 32 + first - 1 + i;
        const uint8_t mask = static_cast<uint8_t>(1u << (7 - bit % 8));
        frame[bit / 8] &= static_cast<uint8_t>(~mask);
        if ((value >> (width - i - 1)) & 1u) frame[bit / 8] |= mask;
    }
}

adsb::Message parsed(const Frame& frame) {
    adsb::Message message;
    require(adsb::parseAdsb(frame.data(), message, 14), "valid fixture parses");
    return message;
}

const Frame ident = fromHex("8D4840D6202CC371C32CE0576098");
const Frame evenPosition = fromHex("8D40621D58C382D690C8AC2863A7");
const Frame oddPosition = fromHex("8D40621D58C386435CC412692AD6");
const Frame velocity = fromHex("8D485020994409940838175B284F");

void testKnownMessages() {
    for (const auto& frame : {ident, evenPosition, oddPosition, velocity})
        require(adsb::crc(frame.data(), 14) == 0, "published message CRC is zero");
    auto msg = parsed(ident);
    require(msg.kind == adsb::KIND_IDENT && msg.icao == 0x4840D6, "identification/address");
    require(msg.callsign == "KLM1023", "published callsign KLM1023");
    require(!msg.hasAltitude && !msg.hasGroundSpeed && !msg.hasHeading, "identification has no position/velocity");

    const auto even = parsed(evenPosition);
    const auto odd = parsed(oddPosition);
    require(even.kind == adsb::KIND_AIRBORNE_POS && even.icao == 0x40621D, "airborne position/address");
    require(!even.cprOddFrame && odd.cprOddFrame, "even and odd CPR flags");
    require(even.cprLat == 93000 && even.cprLon == 51372, "published even CPR integers");
    require(odd.cprLat == 74158 && odd.cprLon == 50194, "published odd CPR integers");
    require(even.hasAltitude && even.altitudeFt == 38000 && odd.altitudeFt == 38000, "Q=1 altitude is 38000 ft");
    double lat = 0, lon = 0;
    require(adsb::cprDecodeGlobal(even.cprLat, even.cprLon, odd.cprLat, odd.cprLon, false, false, lat, lon), "global CPR even latest");
    near(lat, 52.2572021484375, 1e-8, "published even latitude");
    near(lon, 3.91937255859375, 1e-8, "published even longitude");
    require(adsb::cprDecodeGlobal(even.cprLat, even.cprLon, odd.cprLat, odd.cprLon, true, false, lat, lon), "global CPR odd latest");
    near(lat, 52.26578017412606, 1e-8, "published odd latitude");
    near(lon, 3.938912527901786, 1e-8, "published odd longitude");

    msg = parsed(velocity);
    require(msg.kind == adsb::KIND_VELOCITY && msg.icao == 0x485020, "velocity/address");
    require(msg.hasGroundSpeed && !msg.airspeed && msg.hasHeading, "ground velocity flags");
    near(msg.speedKt, 159.201130649, 0.000001, "published ground speed");
    near(msg.headingDeg, 182.880377553, 0.000001, "published ground track");
    require(msg.hasVerticalRate && msg.verticalRateFpm == -832, "published vertical rate -832 ft/min");
}

void requireNoMeasurements(const adsb::Message& msg, const std::string& reason) {
    require(!msg.hasAltitude && !msg.hasGroundSpeed && !msg.hasHeading && !msg.hasVerticalRate && msg.callsign.empty(), reason);
}

void testRejectedFrames() {
    for (int bit = 0; bit < 112; ++bit) {
        auto frame = ident;
        frame[bit / 8] ^= static_cast<uint8_t>(1u << (7 - bit % 8));
        require(adsb::crc(frame.data(), 14) != 0, "single bit error produces nonzero CRC at " + std::to_string(bit));
        auto msg = parsed(velocity); // A rejected frame must not leave old valid fields.
        require(!adsb::parseAdsb(frame.data(), msg, 14), "single bit error rejected");
        requireNoMeasurements(msg, "CRC failure clears old measurements");
    }
    for (int length : {-1, 0, 7, 13, 15}) {
        auto msg = parsed(velocity);
        require(!adsb::parseAdsb(ident.data(), msg, length), "invalid frame length rejected");
        requireNoMeasurements(msg, "invalid length clears old measurements");
    }
    adsb::Message msg;
    require(!adsb::parseAdsb(nullptr, msg, 14), "null input rejected");
    for (int df : {0, 11, 18, 19, 20, 31}) {
        auto frame = ident;
        frame[0] = static_cast<uint8_t>((df << 3) | 5);
        setParity(frame);
        require(!adsb::parseAdsb(frame.data(), msg, 14), "non-DF17 rejected even with zero CRC");
        requireNoMeasurements(msg, "unsupported format has no measurements");
    }
    for (int tc : {0, 23, 28, 31}) {
        auto frame = ident;
        setMe(frame, 1, 5, tc);
        setParity(frame);
        require(!adsb::parseAdsb(frame.data(), msg, 14), "unsupported type code rejected");
        requireNoMeasurements(msg, "unsupported type code has no measurements");
    }
}

void testUnavailableFields() {
    for (uint32_t rawAltitude : {0u, 1u, 0xFEFu}) { // missing or unsupported Gillham Q=0
        auto frame = evenPosition;
        setMe(frame, 9, 12, rawAltitude);
        setParity(frame);
        require(!parsed(frame).hasAltitude, "missing/Q=0 altitude stays invalid");
    }
    auto frame = evenPosition;
    setMe(frame, 1, 5, 20);
    setParity(frame);
    require(!parsed(frame).hasAltitude, "GNSS height is not mislabeled barometric feet");
    frame = velocity;
    setMe(frame, 15, 10, 0);
    setMe(frame, 38, 9, 0);
    setParity(frame);
    auto msg = parsed(frame);
    require(!msg.hasGroundSpeed && !msg.hasHeading && !msg.hasVerticalRate, "unavailable velocity components and rate");
    frame = velocity;
    setMe(frame, 15, 10, 1);
    setMe(frame, 26, 10, 1);
    setParity(frame);
    msg = parsed(frame);
    require(msg.hasGroundSpeed && msg.speedKt == 0, "encoded stationary speed is valid zero");
    require(!msg.hasHeading, "zero ground velocity has no defined track");
    frame = velocity;
    setMe(frame, 6, 3, 2);
    setParity(frame);
    near(parsed(frame).speedKt, parsed(velocity).speedKt * 4.0, 1e-8, "supersonic ground-speed multiplier");
    frame = velocity;
    setMe(frame, 6, 3, 3);
    setMe(frame, 14, 1, 1);
    setMe(frame, 15, 10, 256);
    setMe(frame, 25, 1, 1); // TAS, not IAS
    setMe(frame, 26, 10, 201);
    setParity(frame);
    msg = parsed(frame);
    require(msg.hasGroundSpeed && msg.airspeed && msg.trueAirspeed, "TAS distinguished from ground speed/IAS");
    near(msg.speedKt, 200, 0, "airspeed offset");
    near(msg.headingDeg, 90, 0, "valid magnetic/true heading field angle");
    setMe(frame, 14, 1, 0);
    setMe(frame, 26, 10, 0);
    setParity(frame);
    msg = parsed(frame);
    require(!msg.hasGroundSpeed && !msg.hasHeading, "airspeed and heading unavailable status respected");
    frame = velocity;
    setMe(frame, 6, 3, 0);
    setParity(frame);
    require(!adsb::parseAdsb(frame.data(), msg, 14), "unsupported velocity subtype rejected");
    requireNoMeasurements(msg, "unsupported velocity subtype clears fields");
}

// Independently generate CPR fractions for points in known NL zones.
uint32_t cprFraction(double degrees, double zoneSize) {
    double fraction = degrees / zoneSize;
    fraction -= std::floor(fraction);
    return static_cast<uint32_t>(std::floor(fraction * 131072.0 + 0.5)) & 0x1FFFF;
}

void testCprSafety() {
    double lat = 0, lon = 0;
    require(!adsb::cprDecodeGlobal(93000, 51372, 74158, 50194, false, true, lat, lon), "surface CPR rejected without reference");
    require(!adsb::cprDecodeGlobal(131072, 0, 0, 0, false, false, lat, lon), "out-of-range CPR latitude rejected");
    require(!adsb::cprDecodeGlobal(0, 0, 0, 131072, true, false, lat, lon), "out-of-range CPR longitude rejected");
    require(!adsb::cprDecodeGlobal(93000, 51372, 0, 50194, false, false, lat, lon), "inconsistent CPR latitude pair rejected");
    struct Point { double lat; double lon; int nl; };
    for (const Point point : {Point{-33.9, 151.2, 49}, Point{10, 179.9, 59}, Point{-10, -179.9, 59}}) {
        const auto latE = cprFraction(point.lat, 360.0 / 60.0);
        const auto latO = cprFraction(point.lat, 360.0 / 59.0);
        const auto lonE = cprFraction(point.lon, 360.0 / point.nl);
        const auto lonO = cprFraction(point.lon, 360.0 / (point.nl - 1));
        for (bool odd : {false, true}) {
            require(adsb::cprDecodeGlobal(latE, lonE, latO, lonO, odd, false, lat, lon), "southern/dateline CPR decode");
            near(lat, point.lat, 0.00005, "southern/dateline latitude");
            near(lon, point.lon, 0.00005, "southern/dateline longitude");
        }
    }
    require(adsb::cprNL(0) == 59 && adsb::cprNL(87) == 2 && adsb::cprNL(-88) == 1, "equator and polar NL boundaries");
}

std::vector<float> magnitudeFrame(const Frame& frame) {
    std::vector<float> mag(240, 0.02f);
    for (int pulse : {0, 2, 7, 9}) mag[pulse] = 1.0f;
    for (int bit = 0; bit < 112; ++bit) {
        const bool one = (frame[bit / 8] >> (7 - bit % 8)) & 1;
        mag[16 + 2 * bit + (one ? 0 : 1)] = 1.0f;
    }
    return mag;
}

void expectFrame(const adsb::RawFrame& actual, const Frame& expected, const std::string& label) {
    require(actual.len == 14 && std::memcmp(actual.bytes, expected.data(), 14) == 0, label);
}

std::vector<adsb::RawFrame> detect(const std::vector<float>& samples, int chunkSize) {
    adsb::FrameDetector detector;
    std::vector<adsb::RawFrame> frames;
    detector.onFrame = [&](const adsb::RawFrame& frame) { frames.push_back(frame); };
    for (size_t i = 0; i < samples.size(); i += chunkSize) {
        const int count = static_cast<int>(std::min(static_cast<size_t>(chunkSize), samples.size() - i));
        detector.process(samples.data() + i, count);
    }
    return frames;
}

void testChunkBoundaries() {
    const auto samples = magnitudeFrame(ident);
    // Every split includes split==240, the old bug where a complete frame at the
    // chunk end was retained and decoded a second time when one sample arrived.
    for (size_t split = 0; split <= samples.size(); ++split) {
        adsb::FrameDetector detector;
        std::vector<adsb::RawFrame> frames;
        detector.onFrame = [&](const adsb::RawFrame& frame) { frames.push_back(frame); };
        detector.process(samples.data(), static_cast<int>(split));
        if (split < samples.size()) require(frames.empty(), "incomplete frame waits for all samples");
        detector.process(samples.data() + split, static_cast<int>(samples.size() - split));
        require(frames.size() == 1, "exactly one frame for split " + std::to_string(split));
        expectFrame(frames[0], ident, "reassembled frame matches bytes");
        require(frames[0].sampleOffset == 0, "sample clock unaffected by split location");
        const float quiet = 0.02f;
        for (int i = 0; i < 300; ++i) detector.process(&quiet, 1);
        require(frames.size() == 1, "frame at buffer end is never emitted twice");
    }
    std::vector<float> stream(137, 0.02f);
    for (const auto& frame : {ident, evenPosition, oddPosition, velocity}) {
        const auto mag = magnitudeFrame(frame);
        stream.insert(stream.end(), mag.begin(), mag.end()); // Adjacent, no gap.
    }
    for (int chunkSize : {1, 2, 7, 127, 239, 240, 241, 480, 4096}) {
        const auto frames = detect(stream, chunkSize);
        require(frames.size() == 4, "multiple adjacent frames in arbitrary chunks");
        expectFrame(frames[0], ident, "first multi-frame bytes");
        expectFrame(frames[1], evenPosition, "second multi-frame bytes");
        expectFrame(frames[2], oddPosition, "third multi-frame bytes");
        expectFrame(frames[3], velocity, "fourth multi-frame bytes");
        for (size_t i = 0; i < frames.size(); ++i)
            require(frames[i].sampleOffset == 137 + 240 * i, "sample offsets survive arbitrary chunk sizes");
    }
    adsb::FrameDetector detector;
    int count = 0;
    detector.onFrame = [&](const adsb::RawFrame&) { ++count; };
    detector.process(samples.data(), 120);
    detector.reset();
    detector.process(samples.data() + 120, 120);
    require(count == 0, "reset drops incomplete frame");
    detector.process(samples.data(), 240);
    require(count == 1, "detector receives fresh frame after reset");
    detector.process(nullptr, 0);
    detector.process(nullptr, -1);
    require(count == 1, "empty and invalid chunks do not emit frames");
    detector.reset();
    detector.onFrame = [&](const adsb::RawFrame& frame) { require(frame.sampleOffset == 0, "reset resets sample clock"); };
    detector.process(samples.data(), 240);
}

void testIqAndNoise() {
    // 2 MSps PPM -> synthetic complex baseband -> magnitude -> actual detector.
    // Exercise carrier phase rotations and deterministic additive I/Q noise.
    const auto ideal = magnitudeFrame(velocity);
    for (double phase : {0.0, 0.5, 1.57079632679, 3.14159265359}) {
        for (float amplitude : {0.03f, 1.0f, 12.0f}) {
            std::vector<float> mag;
            for (size_t i = 0; i < ideal.size(); ++i) {
                const double angle = phase + 0.317 * static_cast<double>(i);
                const std::complex<float> iq = std::polar(amplitude * ideal[i], static_cast<float>(angle))
                    + std::complex<float>(amplitude * 0.001f * static_cast<float>(static_cast<int>(i % 5) - 2),
                                          amplitude * 0.001f * static_cast<float>(static_cast<int>(i % 7) - 3));
                mag.push_back(adsb::iqEnvelope(iq.real(), iq.imag()));
            }
            const auto frames = detect(mag, 17);
            require(frames.size() == 1, "synthetic noisy IQ survives carrier rotation/scale");
            expectFrame(frames[0], velocity, "synthetic IQ decoded bytes");
        }
    }
    require(detect(std::vector<float>(4096, 0.0f), 37).empty(), "silence emits no frame");
    require(detect(std::vector<float>(4096, 1.0f), 37).empty(), "constant energy emits no frame");
    auto corrupted = magnitudeFrame(velocity);
    std::swap(corrupted[16 + 60 * 2], corrupted[17 + 60 * 2]);
    require(detect(corrupted, 240).empty(), "detector rejects corrupted frame CRC");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f}) {
        auto mag = ideal;
        mag[16 + 60 * 2] = invalid;
        require(detect(mag, 240).empty(), "invalid magnitude cannot produce valid frame");
    }
    auto tied = ideal;
    tied[16 + 60 * 2] = tied[17 + 60 * 2];
    require(detect(tied, 240).empty(), "equal half-bit samples rejected rather than guessing zero");
    auto badPreamble = ideal;
    badPreamble[7] = 0.02f;
    require(detect(badPreamble, 240).empty(), "missing preamble pulse rejected");
    auto nonAdsb = ident;
    nonAdsb[0] = static_cast<uint8_t>((18 << 3) | 5);
    setParity(nonAdsb);
    require(detect(magnitudeFrame(nonAdsb), 240).empty(), "detector ignores CRC-valid DF18");
}

adsb::Aircraft oneAircraft(adsb::AircraftTracker& tracker, double now) {
    const auto aircraft = tracker.snapshot(now);
    require(aircraft.size() == 1, "tracker contains one aircraft");
    return aircraft.front();
}

void testTrackerCprPrerequisites() {
    const auto even = parsed(evenPosition);
    const auto odd = parsed(oddPosition);
    adsb::AircraftTracker tracker;
    tracker.update(even, 100);
    require(!oneAircraft(tracker, 100).havePos, "one CPR half cannot create valid coordinates");
    tracker.update(odd, 110);
    auto aircraft = oneAircraft(tracker, 110);
    require(aircraft.havePos, "CPR pair exactly 10 seconds apart accepted");
    near(aircraft.lat, 52.26578017412606, 1e-8, "tracker uses most recent odd latitude");
    tracker.clear();
    tracker.update(odd, 100);
    tracker.update(even, 101);
    aircraft = oneAircraft(tracker, 101);
    require(aircraft.havePos, "odd then even pair accepted");
    near(aircraft.lat, 52.2572021484375, 1e-8, "tracker uses most recent even latitude");
    tracker.clear();
    tracker.update(even, 100);
    tracker.update(odd, 110.001);
    require(!oneAircraft(tracker, 110.001).havePos, "CPR halves older than 10 seconds cannot pair");
    tracker.clear();
    tracker.update(even, 100, 0);
    tracker.update(odd, 101, 10.001);
    require(!oneAircraft(tracker, 101).havePos, "accelerated playback respects IQ sample time for CPR");
    tracker.clear();
    tracker.update(even, 100, 0);
    tracker.update(odd, 110, 10);
    require(oneAircraft(tracker, 110).havePos, "10-second sample time boundary accepted");
    tracker.clear();
    tracker.update(even, 100);
    auto otherAircraft = odd;
    ++otherAircraft.icao;
    tracker.update(otherAircraft, 101);
    const auto rows = tracker.snapshot(101);
    require(rows.size() == 2 && !rows[0].havePos && !rows[1].havePos, "different ICAO halves cannot pair");
    tracker.clear();
    tracker.update(even, 100);
    auto gnssOdd = odd;
    gnssOdd.typeCode = 20;
    gnssOdd.hasAltitude = false;
    tracker.update(gnssOdd, 101);
    require(!oneAircraft(tracker, 101).havePos, "barometric/GNSS CPR families cannot pair");
    tracker.clear();
    tracker.update(even, 100);
    tracker.clear();
    tracker.update(odd, 101);
    require(!oneAircraft(tracker, 101).havePos, "clear also discards CPR history");
}

void testTrackerFreshness() {
    auto identMsg = parsed(ident);
    const auto even = parsed(evenPosition);
    const auto odd = parsed(oddPosition);
    auto velocityMsg = parsed(velocity);
    identMsg.icao = velocityMsg.icao = even.icao;
    adsb::AircraftTracker tracker;
    tracker.update(identMsg, 100);
    tracker.update(even, 100);
    tracker.update(odd, 101);
    tracker.update(velocityMsg, 102);
    tracker.update(identMsg, 115); // Live identity must not refresh dynamic data.
    auto aircraft = oneAircraft(tracker, 116);
    require(aircraft.havePos && aircraft.haveAlt && aircraft.haveSpeed && aircraft.haveVrate && aircraft.haveHeading,
            "dynamic measurements remain valid exactly at their TTL");
    require(aircraft.messages == 5 && aircraft.callsign == "KLM1023", "aggregation counts decoded messages and retains callsign");
    aircraft = oneAircraft(tracker, 116.001);
    require(!aircraft.havePos && !aircraft.haveAlt && aircraft.haveSpeed, "position and altitude expire independently of speed");
    aircraft = oneAircraft(tracker, 117.001);
    require(!aircraft.haveSpeed && !aircraft.haveHeading && !aircraft.haveVrate, "velocity flags expire while identity remains live");
    require(aircraft.callsign == "KLM1023", "callsign has its own TTL");
    require(tracker.snapshot(175).size() == 1, "aircraft remains exactly at 60 seconds since last message");
    require(tracker.snapshot(175.001).empty(), "aircraft expires after 60 seconds since last message");
    tracker.update(identMsg, 200);
    for (int time = 230; time <= 500; time += 30) tracker.update(velocityMsg, time);
    require(oneAircraft(tracker, 500).callsign == "KLM1023", "callsign valid exactly at 300 seconds");
    require(oneAircraft(tracker, 500.001).callsign.empty(), "callsign expires despite continued velocity traffic");
    tracker.clear();
    tracker.update(even, 100);
    auto noAltitude = odd;
    noAltitude.hasAltitude = false;
    tracker.update(noAltitude, 101);
    require(!oneAircraft(tracker, 101).haveAlt, "new unavailable altitude invalidates previous altitude immediately");
    tracker.update(velocityMsg, 102);
    auto unavailableVelocity = velocityMsg;
    unavailableVelocity.hasGroundSpeed = unavailableVelocity.hasHeading = unavailableVelocity.hasVerticalRate = false;
    tracker.update(unavailableVelocity, 103);
    aircraft = oneAircraft(tracker, 103);
    require(!aircraft.haveSpeed && !aircraft.haveHeading && !aircraft.haveVrate, "new unavailable velocity invalidates previous fields immediately");

    auto surfaceFrame = evenPosition;
    setMe(surfaceFrame, 1, 5, 5);
    setParity(surfaceFrame);
    const auto surface = parsed(surfaceFrame);
    require(surface.kind == adsb::KIND_SURFACE_POS, "surface recognized as state transition");
    tracker.update(surface, 104);
    aircraft = oneAircraft(tracker, 104);
    require(aircraft.surface && !aircraft.havePos && !aircraft.haveAlt && !aircraft.haveSpeed && !aircraft.haveHeading && !aircraft.haveVrate,
            "surface status invalidates all airborne measurements");
    tracker.update(even, 105);
    require(!oneAircraft(tracker, 105).havePos, "surface transition clears both airborne CPR halves");
    tracker.clear();
    require(tracker.snapshot(106).empty(), "clear removes all aircraft");
    adsb::Message invalid;
    tracker.update(invalid, 106);
    invalid = identMsg;
    invalid.icao = 0;
    tracker.update(invalid, 106);
    tracker.update(identMsg, std::numeric_limits<double>::quiet_NaN());
    require(tracker.snapshot(106).empty(), "invalid identity/time messages cannot create rows");
}

void testTrackerCapacity() {
    adsb::AircraftTracker tracker(2);
    auto message = parsed(ident);
    for (uint32_t address : {3u, 1u, 2u}) {
        message.icao = address;
        tracker.update(message, static_cast<double>(4 - address));
    }
    // Use monotonically increasing time while deliberately unordered ICAO values.
    tracker.clear();
    message.icao = 3;
    tracker.update(message, 1);
    message.icao = 1;
    tracker.update(message, 2);
    message.icao = 2;
    tracker.update(message, 3);
    const auto rows = tracker.snapshot(3);
    require(rows.size() == 2 && rows[0].icao == 1 && rows[1].icao == 2, "capacity evicts oldest row and snapshot sorts ICAO");
}
} // namespace

int main() {
    try {
        testKnownMessages();
        testRejectedFrames();
        testUnavailableFields();
        testCprSafety();
        testChunkBoundaries();
        testIqAndNoise();
        testTrackerCprPrerequisites();
        testTrackerFreshness();
        testTrackerCapacity();
        std::cout << "PASS: " << checks << " ADS-B checks (Release-safe, no assert).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
