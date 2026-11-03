#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/.test-build"
mkdir -p "$BUILD"
CXXFLAGS=(-std=c++14 -O2 -Wall -Wextra -Werror)
/usr/bin/env python3 "$ROOT/Tests/test_tahoe_kc_abi.py"
/usr/bin/env python3 "$ROOT/Tests/test_release_entrypoint_isolation.py"
for compiler in clang++ g++; do
  "$compiler" -std=c++14 -Wall -Wextra -Werror -O2 "$ROOT/Tests/UVC15ProbeOverrunTests.cpp" -o "$BUILD/UVC15ProbeOverrunTests-$compiler"
  "$BUILD/UVC15ProbeOverrunTests-$compiler"
  "$compiler" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCSequoiaProbePatchTests.cpp" -o "$BUILD/UVCSequoiaProbePatchTests-$compiler"
  (cd "$ROOT" && "$BUILD/UVCSequoiaProbePatchTests-$compiler")
done
for cxx in clang++ g++; do
  command -v "$cxx" >/dev/null
  out="$BUILD/UVCProbePatchTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCProbePatchTests.cpp" -o "$out"
  "$out"
  probe="$BUILD/UVCProbeControlTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCProbeControlTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" -o "$probe"
  (cd "$ROOT" && "$probe")
  desc="$BUILD/UVCDescriptorParserTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCDescriptorParserTests.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" -o "$desc"
  (cd "$ROOT" && "$desc")
  engine="$BUILD/UVCProbeEngineTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCProbeEngineTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" -o "$engine"
  (cd "$ROOT" && "$engine")
  context="$BUILD/UVCRecoveryContextTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" "$ROOT/Tests/UVCRecoveryContextTests.cpp" "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" -o "$context"
  (cd "$ROOT" && "$context")
  policy="$BUILD/UVCTransportPolicyTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" \
    "$ROOT/Tests/UVCTransportPolicyTests.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" \
    "$ROOT/UVCProbeFix/UVCRuntimeMode.cpp" "$ROOT/UVCProbeFix/UVCTransportTahoe.cpp" "$ROOT/UVCProbeFix/UVCQuirks.cpp" \
    -o "$policy"
  (cd "$ROOT" && "$policy")
  coordinator="$BUILD/UVCTransportCoordinatorTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" \
    "$ROOT/Tests/UVCTransportCoordinatorTests.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" \
    "$ROOT/UVCProbeFix/UVCRuntimeMode.cpp" "$ROOT/UVCProbeFix/UVCTransportTahoe.cpp" \
    -o "$coordinator"
  (cd "$ROOT" && "$coordinator")
  concurrency="$BUILD/UVCTransportConcurrencyTests-$cxx"
  "$cxx" "${CXXFLAGS[@]}" -pthread \
    "$ROOT/Tests/UVCTransportConcurrencyTests.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeControl.cpp" "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" \
    "$ROOT/UVCProbeFix/UVCProbeEngine.cpp" "$ROOT/UVCProbeFix/UVCRecoveryContext.cpp" \
    "$ROOT/UVCProbeFix/UVCRuntimeMode.cpp" "$ROOT/UVCProbeFix/UVCTransportTahoe.cpp" \
    -o "$concurrency"
  (cd "$ROOT" && "$concurrency")
done
