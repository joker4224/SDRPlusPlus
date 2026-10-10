# Validation record

Validated on Windows x64, 2026-09-27, against SDR++ base
`455b7451fc5d4ce6342216b93517227a81b143ac`.

- Compiler: MSVC 19.50.35728.0, x64, C++17, Release.
- Build: CMake 4.2.3-msvc3, NMake; Windows SDK 10.0.26100.0.
- Local dependencies: GLFW 3.4, VOLK 2.5.2, Zstandard 1.5.7,
  FFTW Windows single-precision library. Dependencies were placed outside the
  source checkout; the plugin adds none.
- Successfully compiled and linked `sdrpp_core.dll`, `adsb_decoder.dll`,
  `file_source.dll`, `sdrpp.exe` and `adsb_tests.exe`.
- CTest from the full SDR++ build: **1/1 passed**.
- Direct Release test execution: **1937 checks passed**, including protocol,
  sample-stream boundaries and aircraft freshness/CPR prerequisites. Checks do
  not depend on `assert` or debug compilation.
- DLL smoke check: Windows `LoadLibrary` resolved the actual core and dependency
  DLLs; `_INFO_`, `_INIT_`, `_CREATE_INSTANCE_`, `_DELETE_INSTANCE_` and `_END_`
  exports were found. Metadata was `adsb_decoder 0.1.0`; `_INIT_`/`_END_` ran.
  This is a load/export check, not an instance lifecycle or GUI reception test.

For the local full build, all unrelated modules were disabled; ADS-B, its tests
and file source were enabled. Normal dependency discovery used
`CMAKE_PREFIX_PATH`. This checkout's Windows build also has fixed PothosSDR
include/library paths, so the local build supplied the alternate VOLK include
directory through `CMAKE_CXX_FLAGS=/I<dependency-prefix>/include` and library
directory through `CMAKE_SHARED_LINKER_FLAGS=/LIBPATH:<dependency-prefix>/lib`
and `CMAKE_EXE_LINKER_FLAGS=/LIBPATH:<dependency-prefix>/lib`. No unrelated core
source or dependency-path changes were committed.

The existing core emitted warnings, including one in the unchanged rigctl
destructor. The final ADS-B module/tests compiled without warnings in their
build output. This does not establish that the wider core is warning-free.

Not validated: actual RF reception, rendered GUI interaction, repeated instance
lifecycle under a live source, Linux/macOS builds, detector sensitivity or
real-time performance on other hardware. The code remains a draft basic
receiver, not an RTM/release-readiness claim.
