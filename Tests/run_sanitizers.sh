#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
BUILD="$ROOT/.test-build/sanitizers"
mkdir -p "$BUILD"
CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null
FLAGS=(-std=c++14 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined)

build_run() {
    local name="$1"; shift
    "$CXX" "${FLAGS[@]}" "$@" -o "$BUILD/$name"
    (cd "$ROOT" && "$BUILD/$name")
}

build_run UVCSequoiaProbePatchTests "$ROOT/Tests/UVCSequoiaProbePatchTests.cpp"
build_run UVCProbeControlTests "$ROOT/Tests/UVCProbeControlTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp"
build_run UVCDescriptorParserTests "$ROOT/Tests/UVCDescriptorParserTests.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp"
build_run UVCProbeEngineTests "$ROOT/Tests/UVCProbeEngineTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" "$ROOT/UVCProbeFix/UVCProbeEngine.cpp"
build_run UVCRecoveryContextTests "$ROOT/Tests/UVCRecoveryContextTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp"
build_run UVCTransportCoordinatorTests "$ROOT/Tests/UVCTransportCoordinatorTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" "$ROOT/UVCProbeFix/UVCRuntimeMode.cpp" "$ROOT/UVCProbeFix/UVCTransportTahoe.cpp"
echo 'ASan+UBSan Sequoia and generic suites: PASS'

TSAN_CXX="${TSAN_CXX:-g++}"
command -v "$TSAN_CXX" >/dev/null
"$TSAN_CXX" -std=c++14 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=thread \
    "$ROOT/Tests/UVCTransportConcurrencyTests.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" \
    "$ROOT/UVCProbeFix/UVCRuntimeMode.cpp" "$ROOT/UVCProbeFix/UVCTransportTahoe.cpp" \
    -o "$BUILD/UVCTransportConcurrencyTests-tsan"
(cd "$ROOT" && "$BUILD/UVCTransportConcurrencyTests-tsan")
echo 'TSan concurrent transport suite: PASS'
