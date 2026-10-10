# USB reconnect and Chinese glyph fixes

The original Android permission receiver unregistered itself after its first
reply and only requested permission in `onCreate`. It retained a single
connection/FD, had no detach cleanup and dereferenced `openDevice()` without a
null check. Native Refresh only read the old cached fields. This is a concrete
software failure path consistent with a device list that stays empty after
replugging; it does not prove that every DiLink USB failure has that cause.

The receiver now lives for the Activity lifetime and observes detach/attach.
DeviceManager reconciles UsbManager's current device list, requests permission
for matching source devices, deduplicates pending requests, supports explicit
Refresh after denial and checks whether device opening actually succeeds.
Sessions are closed on detach/destruction. JNI returns an atomic device snapshot
and correctly restores thread attachment/local-reference state. RTL-SDR acquires
its own duplicate FD for each probe or reception session and closes it after the
native handle. Its source list polls while stopped; a terminated reception
worker permits stopping and refreshing after unplugging. The source menu shows
permission/opening/native-initialization status without requiring ADB.

Chinese fonts previously used Dear ImGui's 2500-character SimplifiedCommon set.
The range now includes BMP ideographs, Extension A, compatibility forms and CJK
punctuation. Android raster size is bounded and retried at smaller sizes if the
atlas exceeds `GL_MAX_TEXTURE_SIZE`; glyph geometry retains the UI size. Smaller
rasterization may soften large text. The selected system font must contain the
glyphs. Supplementary-plane characters above U+FFFF remain outside this build's
16-bit ImWchar support. This does not bundle or guarantee a vendor font.

## Verified on 2026-10-10

- Pure Kotlin USB session tests passed: permission deduplication, denial/Refresh,
  detach/replug, late reply, null-open recovery, session reuse, missed detach,
  destruction and concurrent descriptor acquisition/close.
- MainActivity and DeviceManager compiled against Android SDK 34 and AndroidX
  Core 1.0.1. The project retains its original SDK/Gradle configuration; this
  check does not constitute a Gradle APK build.
- NDK 23.2 generated Android API 28 ARM64 object files for the modified backend,
  RTL-SDR source and font loader, using the source project's RTL-SDR API header.
  Existing warnings remain. No Android shared-library/APK linking was done:
  the project's complete `/sdr-kit` dependency bundle is absent locally.
- An actual ImGui atlas using Microsoft YaHei passed eight common, uncommon,
  traditional and Extension-A glyph checks, advance-cache/Latin-size checks,
  and a 4096 texture bound (actual atlas 4096 x 1936). Android vendor fonts and
  screen appearance have not been tested.
- Windows core/ADS-B module rebuild succeeded; ADS-B CTest remains 1/1 passing
  with the existing 1937 checks.

## Reproduce the focused tests

```sh
kotlinc android/app/src/main/java/DeviceManager.kt android/tests/DeviceManagerTest.kt -include-runtime -d usb-tests.jar
java -jar usb-tests.jar
cmake -S core/tests -B build-font-tests
cmake --build build-font-tests --config Release
# Supply an installed CJK font with the tested glyphs (TTC face zero is used).
build-font-tests/chinese_font_tests root/res/fonts/Roboto-Medium.ttf /path/to/cjk-font.ttf
```

Build the APK with the repository's documented android-sdr-kit/Gradle workflow.
On the vehicle, test first start, Stop/unplug/replug, permission denial then
Refresh/regrant, repeated reconnects and the problematic Chinese labels. Record
the source-menu status if it fails. Cross-test the dongle on a phone/computer and
another Android receiver on the vehicle to distinguish remaining hardware,
power or DiLink driver issues. No live BYD/DiLink verification is claimed.
