#!/usr/bin/env bash
# Reproducible QtPhase Render Policy Arena benchmark runner
#
# Usage:
#   QTBASE_BUILD_DIR=/path/to/build-qtbase \
#   DEMOS_BUILD_DIR=/path/to/build-phase-demos \
#   ./run_arena.sh [policy1 policy2 ...]
#
# Example:
#   QTBASE_BUILD_DIR=../build-qtbase DEMOS_BUILD_DIR=../build-demos \
#     ./run_arena.sh upstream async-race contextual-bandit widget-aware cadence-power

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QTBASE_BUILD_DIR="${QTBASE_BUILD_DIR:?Set QTBASE_BUILD_DIR to your built qtbase directory}"
DEMOS_BUILD_DIR="${DEMOS_BUILD_DIR:?Set DEMOS_BUILD_DIR to your built examples/phase directory}"
OUT_DIR="${OUT_DIR:-${DEMOS_BUILD_DIR}/arena_logs}"
TARGET_DIR="${TARGET_DIR:-${SCRIPT_DIR}/../../../src/widgets/kernel}"
PDF_FILE="${OUT_DIR}/okular_test.pdf"

mkdir -p "${OUT_DIR}"

export LD_LIBRARY_PATH="${QTBASE_BUILD_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${QTBASE_BUILD_DIR}/plugins:/usr/lib64/qt6/plugins:/usr/lib/qt6/plugins"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"

if [ ! -f "${PDF_FILE}" ]; then
  echo "Generating deterministic 20-page test PDF at ${PDF_FILE}..."
  QT_QPA_PLATFORM=offscreen "${DEMOS_BUILD_DIR}/gen_test_pdf" "${PDF_FILE}"
fi

if [ "$#" -gt 0 ]; then
  POLICIES=("$@")
else
  POLICIES=(
    "upstream"
    "async-race"
    "dynamic-hybrid"
    "amortized-hybrid"
    "cadence-power"
    "upload-aware"
    "contextual-bandit"
    "widget-aware"
    "cpu-only"
    "gpu-only"
  )
fi

for pol in "${POLICIES[@]}"; do
  echo "=== Running policy: ${pol} ==="
  if [ "$pol" = "upstream" ]; then
    export QT_WIDGETS_PHASE=0
    unset QT_PHASE_POLICY QT_WIDGETS_PHASE_STATS QT_WIDGETS_PHASE_PROFILE || true
  else
    export QT_WIDGETS_PHASE=1
    export QT_PHASE_POLICY="$pol"
    export QT_WIDGETS_PHASE_STATS=1
    export QT_WIDGETS_PHASE_PROFILE=1
  fi

  # 1. Adversarial 4-Phase Suite (Resize + Sparse Typing + Dynamic Fallback + Popup Churn)
  "${DEMOS_BUILD_DIR}/bench_adversarial" 2>"${OUT_DIR}/arena_adv_${pol}.log"

  # 2. Konsole (if installed)
  if command -v konsole >/dev/null 2>&1; then
    QT_KONSOLE_BENCH=1 LD_PRELOAD="${DEMOS_BUILD_DIR}/bench_konsole_preload.so" \
      konsole --nofork -e bash -c "seq 1 80 | xargs -I{} echo 'Line {}: [QtPhase] Dense monospace Konsole benchmark row 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ'; sleep 10" \
      2>"${OUT_DIR}/arena_konsole_${pol}.log" || true
  fi

  # 3. Dolphin (if installed)
  if command -v dolphin >/dev/null 2>&1; then
    QT_DOLPHIN_BENCH=1 LD_PRELOAD="${DEMOS_BUILD_DIR}/bench_dolphin_preload.so" \
      dolphin --new-window "${TARGET_DIR}" 2>"${OUT_DIR}/arena_dolphin_${pol}.log" || true
  fi

  # 4. Okular (if installed)
  if command -v okular >/dev/null 2>&1; then
    QT_OKULAR_BENCH=1 LD_PRELOAD="${DEMOS_BUILD_DIR}/bench_okular_preload.so" \
      okular --noraise "${PDF_FILE}" 2>"${OUT_DIR}/arena_okular_${pol}.log" || true
  fi

  echo "Saved logs to ${OUT_DIR}/arena_*_${pol}.log"
done

echo ""
echo "=== Summary ==="
grep -hE "SUMMARY|PhaseRenderPolicy" "${OUT_DIR}"/arena_*.log || true
