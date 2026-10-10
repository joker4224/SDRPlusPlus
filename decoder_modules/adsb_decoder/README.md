# Native ADS-B receiver (basic version)

Receives 1090 MHz ADS-B from SDR++'s existing IQ signal path. SDR++ owns the
receiver, tuning, sample rate and gain. This module creates a 2 MS/s VFO, decodes
the IQ envelope and displays an ImGui aircraft table. No external decoder,
network service, receiver driver or new library dependency is required by the
module itself. It still requires the normal SDR++ build dependencies.

The decoder is ported from SwissKnifeEdition; see [source and license](THIRD_PARTY.md).

## Build

From an SDR++ checkout with its normal build dependencies installed:

```sh
cmake -S . -B build -DOPT_BUILD_ADSB_DECODER=ON -DOPT_BUILD_ADSB_TESTS=ON
cmake --build build --config Release --target adsb_decoder adsb_tests
ctest --test-dir build -C Release --output-on-failure
```

Keep your usual platform toolchain, dependency paths and other module options.
With CMake 4, older bundled dependencies may also need
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
`OPT_BUILD_ADSB_DECODER` defaults to OFF; building the tests is optional.
The native module follows `sdrpp_module.cmake`'s existing install rules.

The protocol, stream and tracker tests can also be built **without SDR++ or its
libraries**, with any C++17 compiler:

```sh
cmake -S decoder_modules/adsb_decoder/tests -B build-adsb-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-adsb-tests --config Release
ctest --test-dir build-adsb-tests -C Release --output-on-failure
```

## Enable and receive

1. Use the SDR++ core built from this checkout. Put `adsb_decoder.dll` (Windows),
   `adsb_decoder.so` (Linux) or `adsb_decoder.dylib` (macOS) into the configured
   `modulesDirectory`. For development, alternatively add its full path to the
   `modules` array in the SDR++ root's `config.json` while SDR++ is closed.
   Do not assume binary compatibility with an unrelated nightly build.
2. Start SDR++. In **Module Manager**, create an instance of `adsb_decoder`, for
   example named `ADS-B`. One instance is supported. The left-menu checkbox
   enables/disables it.
3. Select your existing source, set an effective rate of **at least 2 MS/s after
   decimation**, and use a suitable 1090 MHz antenna. Click **Tune to 1090 MHz**
   in the ADS-B menu, then SDR++'s **Play** button. This tuning button centers the
   receiver and affects other VFOs sharing the receiver. Adjust source gain as
   needed; the plugin does not change gain or select hardware.
4. Open **Show aircraft list**. A callsign may arrive later than position or
   velocity. Positions stay `--` until a usable odd/even airborne CPR pair arrives.

For an explicit config instance, merge this entry into `moduleInstances`:

```json
"ADS-B": { "module": "adsb_decoder", "enabled": true }
```

If the table remains empty, first check that the menu reports **Receiving
1090 MHz**, the source is playing, the VFO is centered on 1090 MHz, the full
2 MHz channel fits within the source bandwidth, and CRC-valid frame counts
increase. Correct tuning alone does not guarantee reception.

## Actual scope and validity

- Only directly transmitted, CRC-valid **DF17** frames are accepted, with no
  bit-error correction. A corrupt frame cannot update aircraft state.
- TC 1–4: broadcast callsign. It is **not a verified commercial flight number**.
- TC 9–18: airborne position and Q=1 barometric altitude, displayed in feet.
  Q=0/Gillham altitude remains unknown. TC 20–22 can contribute airborne CPR
  position, but GNSS height remains unknown. Height sources are not mixed.
- TC 19: ground speed / IAS / TAS in knots, ground track or heading in degrees,
  and vertical rate in ft/min with its barometric/GNSS source marked.
- Global airborne CPR requires the same ICAO address, matching altitude-source
  family, valid latitude zones, and odd/even frames no more than 10 seconds apart
  in both reception time and IQ sample time. There is no single-frame local fix.
- TC 5–8 only clears airborne state and marks a surface transition. Surface
  positions/speeds, DF18/TIS-B, Mode A/C, MLAT, maps and network output are omitted.
- Position, altitude and velocity expire independently after 15 seconds;
  callsign after 300 seconds; aircraft after 60 seconds without recognized
  messages. A new explicitly unavailable measurement clears that field sooner.
  The table holds at most 512 aircraft. `--` never means zero.
- Stop/start, disable/enable, tuning changes and long input gaps discard partial
  frames and tracked state. GUI and DSP access share a mutex; the GUI draws a
  copied snapshot. CPR uses sample offsets to avoid pairing distant messages
  during accelerated IQ playback. Display ages are elapsed reception time, not
  historical recording timestamps.

This is a basic two-sample-per-bit detector. Sensitivity under weak signals,
fractional timing, overlapping transmissions and real tuner impairments has not
been established. Synthetic IQ tests do not replace live RF validation. No
hardware reception, operational accuracy or release/RTM readiness is claimed.

## Regression coverage

Release-safe tests cover known callsign/altitude/velocity/global-CPR messages,
each single-bit corruption, unsupported/short frames, unavailable measurements,
all 241 split points of a 240-sample frame, exact-tail duplicate prevention,
single-sample/multi-frame streams, sample offsets, reset, synthetic IQ with
phase/amplitude/noise variations, malformed samples, incompatible/stale CPR,
accelerated playback, per-field expiry, surface transitions and bounded storage.
