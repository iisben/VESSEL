#!/usr/bin/env bash
# Native Linux x86_64 Release build, QA, local install (--install), and packager for VESSEL 2.0.
# Required packages on Ubuntu 24.04 / Debian:
#   sudo apt-get install -y build-essential cmake ninja-build pkg-config \
#     libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev \
#     libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
#     libxrandr-dev libxrender-dev libgl1-mesa-dev libgl1-mesa-dri \
#     libglu1-mesa-dev libjack-jackd2-dev ladspa-sdk
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-linux}"
SKIP_QA="${SKIP_QA:-0}"
DO_INSTALL=0

for arg in "$@"; do
  case "$arg" in
    --install) DO_INSTALL=1 ;;
    --skip-qa) SKIP_QA=1 ;;
  esac
done

cd "${REPO_ROOT}"

VERSION="$(grep -E '^project\(VESSEL VERSION ' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS=(-G Ninja)
fi

JUCE_ARGS=()
if [[ -n "${VESSEL_JUCE_PATH:-}" ]]; then
  JUCE_ARGS=("-DVESSEL_JUCE_PATH=${VESSEL_JUCE_PATH}")
fi

echo "==> Configuring VESSEL ${VERSION} (Linux x86_64 Release)..."
cmake -S . -B "${BUILD_DIR}" "${GENERATOR_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DVESSEL_BUILD_QA=ON \
  "${JUCE_ARGS[@]}"

if [[ -f "${BUILD_DIR}/compile_commands.json" ]]; then
  ln -sf "${BUILD_DIR}/compile_commands.json" "${REPO_ROOT}/compile_commands.json"
fi

echo "==> Building Release VST3, Standalone & QA suites..."
cmake --build "${BUILD_DIR}" --config Release --parallel \
  --target Vessel_VST3 Vessel_Standalone Vessel2QA Vessel2PresetQA

if [[ "${SKIP_QA}" != "1" ]]; then
  echo "==> Running 8-Gate DSP & Control-Plane QA..."
  "${BUILD_DIR}/Vessel2QA_artefacts/Release/Vessel2QA" --strict --stage4-qa

  echo "==> Running 20-Preset Audio QA..."
  "${BUILD_DIR}/Vessel2PresetQA_artefacts/Release/Vessel2PresetQA" --no-wav

  if command -v pluginval >/dev/null 2>&1; then
    echo "==> Running pluginval (strictness level 5)..."
    if command -v xvfb-run >/dev/null 2>&1 && [[ -z "${DISPLAY:-}" ]]; then
      LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 1400x900x24" \
        pluginval --strictness-level 5 --validate-in-process --timeout-ms 120000 \
        --validate "${BUILD_DIR}/Vessel_artefacts/Release/VST3/VESSEL.vst3"
    else
      pluginval --strictness-level 5 --validate-in-process --timeout-ms 120000 \
        --validate "${BUILD_DIR}/Vessel_artefacts/Release/VST3/VESSEL.vst3"
    fi
  else
    echo "Note: pluginval not found on PATH; skipping pluginval."
  fi
fi

if [[ "${DO_INSTALL}" == "1" ]]; then
  echo "==> Installing VST3 to ~/.vst3/VESSEL.vst3..."
  mkdir -p "${HOME}/.vst3"
  rm -rf "${HOME}/.vst3/VESSEL.vst3"
  cp -r "${BUILD_DIR}/Vessel_artefacts/Release/VST3/VESSEL.vst3" "${HOME}/.vst3/"
fi

echo "==> Packaging dist/linux/VESSEL-${VERSION}-Linux-x86_64.tar.gz..."
STAGE="${REPO_ROOT}/dist/linux/VESSEL-${VERSION}-Linux-x86_64"
ARCHIVE="${REPO_ROOT}/dist/linux/VESSEL-${VERSION}-Linux-x86_64.tar.gz"
rm -rf "${STAGE}" "${ARCHIVE}"
mkdir -p "${STAGE}/Standalone"

cp -r "${BUILD_DIR}/Vessel_artefacts/Release/VST3/VESSEL.vst3" "${STAGE}/"
cp "${BUILD_DIR}/Vessel_artefacts/Release/Standalone/VESSEL" "${STAGE}/Standalone/"
cp -r "${BUILD_DIR}/Vessel_artefacts/Release/Standalone/Wavetables" "${STAGE}/Standalone/"
cp "${REPO_ROOT}/docs/INSTALL-Linux.txt" "${STAGE}/INSTALL-Linux.txt"

tar -czf "${ARCHIVE}" -C "${REPO_ROOT}/dist/linux" "VESSEL-${VERSION}-Linux-x86_64"
rm -rf "${STAGE}"

echo "DONE: ${ARCHIVE}"
