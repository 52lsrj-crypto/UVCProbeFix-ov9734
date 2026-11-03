#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
BUILD="$ROOT/.test-build/fuzz"
CORPUS="$BUILD/corpus"
CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null
rm -rf "$BUILD"
mkdir -p "$BUILD" "$CORPUS/probe" "$CORPUS/descriptors"
COMMON_FLAGS=(-std=c++14 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer)
SANITIZER_FLAGS=(-fsanitize=address,undefined)
MODE="standalone sanitizer fuzz"

# Apple toolchains do not always ship libclang_rt.fuzzer_osx.a. Probe the
# runtime separately so a missing optional libFuzzer archive does not block
# the release gate. Production targets still compile with ASan+UBSan and run
# the same bounded corpus/runs through an in-process deterministic driver.
cat > "$BUILD/libfuzzer_probe.cpp" <<'PROBE'
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }
PROBE

if [[ "${UVC_FORCE_STANDALONE_FUZZ:-0}" != "1" ]] && \
   "$CXX" -std=c++14 -fsanitize=fuzzer "$BUILD/libfuzzer_probe.cpp" -o "$BUILD/libfuzzer_probe" \
       >"$BUILD/libfuzzer-probe.stdout" 2>"$BUILD/libfuzzer-probe.stderr"; then
  MODE="libFuzzer"
  FUZZ_FLAGS=(-fsanitize=fuzzer,address,undefined)
  EXTRA_SOURCES=()
else
  printf 'libFuzzer runtime unavailable; using deterministic in-process sanitizer fuzz fallback\n'
  FUZZ_FLAGS=("${SANITIZER_FLAGS[@]}")
  EXTRA_SOURCES=("$ROOT/Tests/Fuzz/fuzz_standalone_main.cpp")
fi

"$CXX" "${COMMON_FLAGS[@]}" "${FUZZ_FLAGS[@]}" \
  "$ROOT/Tests/Fuzz/fuzz_uvc_probe.cpp" \
  "$ROOT/UVCProbeFix/UVCProbeControl.cpp" \
  "${EXTRA_SOURCES[@]}" \
  -o "$BUILD/fuzz_uvc_probe"

"$CXX" "${COMMON_FLAGS[@]}" "${FUZZ_FLAGS[@]}" \
  "$ROOT/Tests/Fuzz/fuzz_uvc_descriptors.cpp" \
  "$ROOT/UVCProbeFix/UVCDescriptorParser.cpp" \
  "${EXTRA_SOURCES[@]}" \
  -o "$BUILD/fuzz_uvc_descriptors"

cp -f "$ROOT"/Tests/Fixtures/*probe.bin "$CORPUS/probe/" 2>/dev/null || true
cp -f "$ROOT"/Tests/Fixtures/*descriptors*.bin "$ROOT"/Tests/Fixtures/synthetic-endpoint-*.bin "$CORPUS/descriptors/" 2>/dev/null || true

"$BUILD/fuzz_uvc_probe" -seed=2303 -runs=20000 -max_len=64 "$CORPUS/probe"
"$BUILD/fuzz_uvc_descriptors" -seed=2303 -runs=20000 -max_len=4096 "$CORPUS/descriptors"
printf '%s+ASan+UBSan smoke: PASS\n' "$MODE"
