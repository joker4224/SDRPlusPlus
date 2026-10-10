# Source and license

This module is a focused port of the native ADS-B receiver in
[F4JTV/SDRPlusPlus_SwissKnifeEdition](https://github.com/F4JTV/SDRPlusPlus_SwissKnifeEdition/tree/bb6351b98b58b724949c017e42d312030fbbd3f3/decoder_modules/adsb_decoder),
revision `bb6351b98b58b724949c017e42d312030fbbd3f3`.
The source module credits **SDR++ Community**. Its repository supplies the GNU
GPL version 3 in its root `license`; this repository carries that license too.

The following implementation was reused, rather than independently rewritten:

- `src/adsb/adsb.h` and `adsb.cpp`: Mode S CRC, identification, Q=1 altitude,
  airborne velocity and global CPR calculations.
- `src/adsb/frame_detector.h`: 2 MS/s magnitude-domain preamble detection and PPM slicing.
- The native VFO/lifecycle integration and aircraft aggregation in the source
  `src/main.cpp`, `decoder.h` and `dsp.h` informed this port's smaller `main.cpp`
  and `aircraft_tracker.h`.

Changes in this port: strict DF17/length/CRC checks; consume exactly the scanned
sample prefix to avoid repeated frames at chunk boundaries; retain a sample
offset for CPR timing; reject invalid numeric samples; use portable PI; mark
IAS/TAS and vertical-rate sources; require compatible, fresh airborne CPR
pairs; expire fields separately; bound aircraft storage; synchronize the native
IQ sink and GUI snapshots. The original TCP output, local/surface position
decoding and optional reference-position configuration are not included.

`lankalana/SDRPlusPlusBrown` and `antirez/dump1090` were examined during selection;
their source is not bundled in this port. The retained detector's description
of the classic dump1090 approach comes from the SwissKnife source.

The tests use the published examples from Junzi Sun's *The 1090 Megahertz Riddle*:
[identification](https://mode-s.org/1090mhz/content/ads-b/2-identification.html),
[airborne position](https://mode-s.org/1090mhz/content/ads-b/3-airborne-position.html),
and [airborne velocity](https://mode-s.org/1090mhz/content/ads-b/5-airborne-velocity.html).
Additional test inputs are synthetic frames and IQ envelopes.
