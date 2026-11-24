# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

#####################################################################
## Phase Module:
#####################################################################

find_package(PkgConfig REQUIRED)
pkg_check_modules(HB_PHASE REQUIRED IMPORTED_TARGET harfbuzz-gpu harfbuzz-raster harfbuzz)
find_package(Vulkan REQUIRED)

find_program(QT_PHASE_QSB_EXECUTABLE NAMES qsb-qt6 qsb HINTS /usr/bin /usr/lib64/qt6/bin REQUIRED)

function(qt_phase_compile_shader TARGET_HEADER_VAR SHADER_SRC ARRAY_NAME)
    get_filename_component(SHADER_NAME "${SHADER_SRC}" NAME)
    set(QSB_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.qsb")
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
    string(REPLACE "." "_" HEADER_STEM "${SHADER_NAME}")
    set(HDR_OUT "${CMAKE_CURRENT_BINARY_DIR}/${HEADER_STEM}_spv.h")

    add_custom_command(
        OUTPUT "${HDR_OUT}"
        COMMAND "${QT_PHASE_QSB_EXECUTABLE}" -O -o "${QSB_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}"
        COMMAND "${QT_PHASE_QSB_EXECUTABLE}" -x spirv,100 -o "${SPV_OUT}" "${QSB_OUT}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT_SPV=${SPV_OUT}"
            "-DOUTPUT_HEADER=${HDR_OUT}"
            "-DARRAY_NAME=${ARRAY_NAME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSpirvHeader.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSpirvHeader.cmake"
        COMMENT "Compiling QtPhase Vulkan 1.3 shader ${SHADER_SRC}"
        VERBATIM
    )
    set(${TARGET_HEADER_VAR} "${HDR_OUT}" PARENT_SCOPE)
endfunction()

qt_phase_compile_shader(PHASE_VERT_HDR "shaders/phase_uber.vert" "kPhaseVertSpirv")
qt_phase_compile_shader(PHASE_FRAG_HDR "shaders/phase_uber.frag" "kPhaseFragSpirv")
qt_phase_compile_shader(PHASE_PRIM_PREP_HDR "shaders/phase_prim_prep.comp" "kPhasePrimPrepCompSpirv")
qt_phase_compile_shader(PHASE_TILE_BIN_HDR "shaders/phase_tile_bin.comp" "kPhaseTileBinCompSpirv")
qt_phase_compile_shader(PHASE_GLYPH_BAKE_HDR "shaders/phase_glyph_bake.comp" "kPhaseGlyphBakeCompSpirv")

qt_internal_add_module(Phase
    SOURCES
        qtphaseglobal.h
        qphasestyles.h
        qphasescenearena_p.h qphasescenearena.cpp
        qphasevisuals_p.h qphasevisuals.h qphasevisuals.cpp
        qphaseview.h qphaseview.cpp
        qphaserenderpolicy_p.h qphaserenderpolicy.cpp
        qphasesurfacepresenter_p.h qphasesurfacepresenter.cpp
        qphasewindow.h qphasewindow.cpp
        widgets/qphaselabel.h widgets/qphaselabel.cpp
        widgets/qphasetextedit.h widgets/qphasetextedit.cpp
        widgets/qphasetableview.h widgets/qphasetableview.cpp
        cpu/qphasesharedmath_p.h
        cpu/qphasecpurasterizer_p.h cpu/qphasecpurasterizer.cpp
        vulkan/qphasevma.cpp
        vulkan/qphasevulkanbackend_p.h vulkan/qphasevulkanbackend.cpp
        "${PHASE_VERT_HDR}"
        "${PHASE_FRAG_HDR}"
        "${PHASE_PRIM_PREP_HDR}"
        "${PHASE_TILE_BIN_HDR}"
        "${PHASE_GLYPH_BAKE_HDR}"
    DEFINES
        QT_NO_CONTEXTLESS_CONNECT
        QT_NO_USING_NAMESPACE
        QT_NO_URL_CAST_FROM_STRING
    INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_BINARY_DIR}"
        widgets
    LIBRARIES
        Qt::CorePrivate
        Qt::GuiPrivate
        Vulkan::Vulkan
        PkgConfig::HB_PHASE
    PUBLIC_LIBRARIES
        Qt::Core
        Qt::Gui
    PRIVATE_MODULE_INTERFACE
        Qt::CorePrivate
        Qt::GuiPrivate
)
