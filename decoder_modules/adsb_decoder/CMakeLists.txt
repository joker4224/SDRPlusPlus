cmake_minimum_required(VERSION 3.13)
project(adsb_decoder LANGUAGES CXX)

set(SRC src/main.cpp src/adsb/adsb.cpp)
include(${SDRPP_MODULE_CMAKE})
target_include_directories(adsb_decoder PRIVATE "src/")
target_compile_features(adsb_decoder PRIVATE cxx_std_17)

if(OPT_BUILD_ADSB_TESTS)
    add_subdirectory(tests)
endif()
