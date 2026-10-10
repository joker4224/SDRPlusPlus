cmake_minimum_required(VERSION 3.13)
project(chinese_font_tests LANGUAGES CXX)
add_executable(chinese_font_tests chinese_font_tests.cpp
    ../src/imgui/imgui.cpp ../src/imgui/imgui_draw.cpp
    ../src/imgui/imgui_tables.cpp ../src/imgui/imgui_widgets.cpp)
target_include_directories(chinese_font_tests PRIVATE ../src ../src/imgui)
target_compile_features(chinese_font_tests PRIVATE cxx_std_17)
