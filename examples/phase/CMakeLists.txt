cmake_minimum_required(VERSION 3.22)
project(QtPhaseDemosAndBenchmarks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_NO_PRIVATE_MODULE_WARNING ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Phase PhasePrivate)
find_package(Vulkan REQUIRED)

get_target_property(QT_CORE_SONAME_PATH Qt6::Core LOCATION)
get_filename_component(QT_BUILD_LIB_DIR "${QT_CORE_SONAME_PATH}" DIRECTORY)
get_filename_component(QT_BUILD_ROOT_DIR "${QT_BUILD_LIB_DIR}" DIRECTORY)
set(QT_PHASE_PLUGINS_DIR "${QT_BUILD_ROOT_DIR}/plugins")

# ----------------------------------------------------------------------------
# 1. Demos & Pixel-Parity Verification
# ----------------------------------------------------------------------------

# 7-stage CPU <-> Vulkan 1.3 pixel parity, curve arena, shared VkDevice & affinity verifier
add_executable(parity_diff
    parity_diff/main.cpp
)
target_compile_definitions(parity_diff PRIVATE
    QT_PHASE_PLUGINS_DIR="${QT_PHASE_PLUGINS_DIR}"
)
target_link_libraries(parity_diff PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Phase
    Qt6::PhasePrivate
    Vulkan::Vulkan
)

# Native QtPhase showcase (100k-row virtualized table, hb-gpu text, embedded Vulkan 3D pass)
add_executable(phase_demo
    phase_demo/main.cpp
)
target_compile_definitions(phase_demo PRIVATE
    QT_PHASE_PLUGINS_DIR="${QT_PHASE_PLUGINS_DIR}"
)
target_link_libraries(phase_demo PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Phase
    Vulkan::Vulkan
)

# Standard Qt Widgets app (ZERO QtPhase includes or APIs — runs transparently on QtPhase!)
add_executable(widgets_phase_demo
    widgets_phase_demo/main.cpp
)
target_compile_definitions(widgets_phase_demo PRIVATE
    QT_PHASE_PLUGINS_DIR="${QT_PHASE_PLUGINS_DIR}"
)
target_link_libraries(widgets_phase_demo PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# ----------------------------------------------------------------------------
# 2. Reproducible Policy Arena Benchmarks
# ----------------------------------------------------------------------------

# 4-phase adversarial stress suite (Split resize + sparse typing + dynamic fallback + popup churn)
add_executable(bench_adversarial
    benchmarks/bench_adversarial.cpp
)
target_link_libraries(bench_adversarial PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Deterministic 20-page PDF generator for Okular scroll/resize benchmarks
add_executable(gen_test_pdf
    benchmarks/gen_test_pdf.cpp
)
target_link_libraries(gen_test_pdf PRIVATE
    Qt6::Core
    Qt6::Gui
)

# LD_PRELOAD benchmark drivers for unmodified KDE applications (Dolphin, Konsole, Okular)
add_library(bench_dolphin_preload SHARED
    benchmarks/bench_dolphin_preload.cpp
)
set_target_properties(bench_dolphin_preload PROPERTIES PREFIX "")
target_link_libraries(bench_dolphin_preload PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

add_library(bench_konsole_preload SHARED
    benchmarks/bench_konsole_preload.cpp
)
set_target_properties(bench_konsole_preload PROPERTIES PREFIX "")
target_link_libraries(bench_konsole_preload PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

add_library(bench_okular_preload SHARED
    benchmarks/bench_okular_preload.cpp
)
set_target_properties(bench_okular_preload PROPERTIES PREFIX "")
target_link_libraries(bench_okular_preload PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
