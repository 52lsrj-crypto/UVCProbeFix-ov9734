#!/bin/bash
set -euo pipefail
IFS=$'\n\t'
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
VERSION="0.3.3"
PYTHON="$(command -v python3 || true)"
[ -n "$PYTHON" ] || { echo 'ERROR: python3 is required for source validation' >&2; exit 1; }

"$PYTHON" - "$ROOT" <<'PY'
import hashlib
import pathlib
import plistlib
import re
import sys

root=pathlib.Path(sys.argv[1])
version='0.3.3'
build_number='303'

with (root/'UVCProbeFix'/'Info.plist').open('rb') as f:
    p=plistlib.load(f)
assert p['CFBundleIdentifier']=='com.dnmtechlabs.UVCProbeFix'
assert p['CFBundleExecutable']=='UVCProbeFix'
assert p['CFBundleShortVersionString']==version
assert p['CFBundleVersion']==build_number
assert p['CFBundleGetInfoString']=='UVCProbeFix 0.3.3 (303)'
assert p['NSHumanReadableCopyright']=='Copyright © 2026 DNMTechLabs. All rights reserved. dnmtechlabs@gmail.com'
assert p['OSBundleLibraries']['as.vit9696.Lilu']=='1.7.2'

# 174F:1176 merge properties are retained byte-for-byte in meaning only as
# legacy bootstrap compatibility; generic policy is audited separately below.
ps=p['IOKitPersonalities']
assert set(ps)=={'UVCProbeFix','UVCProbeFix-174F1176-Control','UVCProbeFix-174F1176-Streaming'}
for name,iface,sub in (('UVCProbeFix-174F1176-Control',0,1),('UVCProbeFix-174F1176-Streaming',1,2)):
    q=ps[name]
    assert q['CFBundleIdentifier']=='com.apple.driver.AppleUSBHostMergeProperties'
    assert q['idVendor']==5967 and q['idProduct']==4470
    assert q['bConfigurationValue']==1 and q['bInterfaceClass']==14
    assert q['bInterfaceNumber']==iface and q['bInterfaceSubClass']==sub
    assert q['IOProviderMergeProperties']['CameraAssistantBundleID']=='com.apple.cmio.uvcassistantextension'

with (root/'OpenCore-Kernel-Add-entry.plist').open('rb') as f:
    oc=plistlib.load(f)
assert oc['Arch']=='x86_64' and oc['BundlePath']=='UVCProbeFix.kext'
assert oc['MinKernel']=='24.0.0' and oc['MaxKernel']=='25.99.99' and oc['Enabled'] is True
assert version in oc['Comment']

expected_runtime={
 'Info.plist','UVCProbePatch.hpp','kern_start.cpp','kern_start_generic.cpp',
 'UVCLegacy0215.hpp','UVCLegacy0215.cpp','UVCRuntimeMode.hpp','UVCRuntimeMode.cpp',
 'UVCProbeControl.hpp','UVCProbeControl.cpp','UVCDescriptorParser.hpp','UVCDescriptorParser.cpp',
 'UVCProbeEngine.hpp','UVCProbeEngine.cpp','UVCRecoveryContext.hpp','UVCRecoveryContext.cpp',
 'UVCTransportTahoe.hpp','UVCTransportTahoe.cpp','UVCQuirks.hpp','UVCQuirks.cpp',
 'UVC15ProbeOverrun.hpp','UVC15ProbeOverrun.cpp',
 'UVCSequoia.hpp','UVCSequoia.cpp','UVCSequoiaProbePatch.hpp'
}
runtime={x.name for x in (root/'UVCProbeFix').iterdir() if x.is_file()}
assert runtime==expected_runtime,(sorted(runtime),sorted(expected_runtime))

# Kernel target is compiled with -nostdinc; production sources must not depend
# on libc++ C++ wrapper headers or std:: C-library wrappers.
forbidden_kernel_headers=('cstddef','cstdint','cstring','limits','array','vector','string','memory','algorithm','utility','type_traits','atomic','mutex','thread')
for path in sorted((root/'UVCProbeFix').glob('*')):
    if path.suffix not in {'.hpp','.cpp'}:
        continue
    text=path.read_text()
    for header in forbidden_kernel_headers:
        assert f'#include <{header}>' not in text,(path.name,header)
    assert not re.search(r'\bstd::(?:memcpy|memcmp|memset|strcmp|numeric_limits)\b',text),path.name

# Exact 0.2.15 policy bytes remain frozen for rollback.
legacy_patch=(root/'UVCProbeFix'/'UVCProbePatch.hpp').read_bytes()
assert hashlib.sha256(legacy_patch).hexdigest()=='35dbb728de7e20ab0068f86bb304ad84776dcf7c70a31a347a93e20622ef44e8'

pbx=(root/'UVCProbeFix.xcodeproj'/'project.pbxproj').read_text()
assert pbx.count('MODULE_VERSION = 0.3.3;')==2
assert pbx.count('CURRENT_PROJECT_VERSION = 303;')==2
assert pbx.count('MARKETING_VERSION = 0.3.3;')==2
for source in ('kern_start.cpp','kern_start_generic.cpp','UVCLegacy0215.cpp','UVCRuntimeMode.cpp','UVCProbeControl.cpp',
               'UVCDescriptorParser.cpp','UVCProbeEngine.cpp','UVCRecoveryContext.cpp',
               'UVCTransportTahoe.cpp','UVCQuirks.cpp','UVC15ProbeOverrun.cpp','UVCSequoia.cpp'):
    assert f'{source} in Sources' in pbx,source
assert pbx.count('"UVCP_KERNEL_BUILD=1"')==2
assert pbx.count('ARCHS = x86_64;')>=2
# Every PBXBuildFile source reference must point to a declared PBXFileReference.
used=set(re.findall(r'fileRef = (A2[0-9A-F]{22})',pbx))
declared=set(re.findall(r'^\s*(A2[0-9A-F]{22}) /\* .*? \*/ = \{isa = PBXFileReference;',pbx,re.M))
assert not (used-declared),sorted(used-declared)
# Removing only the marked additions and config lower-bound change restores
# the physically-proven 0.2.15 Tahoe entrypoint byte-for-byte.
release_kern=(root/'UVCProbeFix'/'kern_start.cpp').read_bytes()
assert 'UVC15::start()' in release_kern.decode()
assert 'UVCSequoia::start()' in release_kern.decode()
assert (root/'Tests'/'test_release_entrypoint_isolation.py').is_file()
import subprocess
subprocess.run([sys.executable,str(root/'Tests'/'test_release_entrypoint_isolation.py')],check=True)
assert (root/'Tests'/'test_release_entrypoint_isolation.py').is_file()
# Debug/Test uses a separate dispatcher; Release excludes every generic/split unit.
generic_kern=(root/'UVCProbeFix'/'kern_start_generic.cpp').read_text()
assert 'UVC::startTahoeTransport(mode)' in generic_kern
assert 'kGenericDefaultEnabled = false' in generic_kern
assert 'UVCLegacy0215::start()' in generic_kern
assert 'UVCSequoia::start()' in generic_kern
assert '-uvcplegacy' in generic_kern and '-uvcprecover' in generic_kern and '-uvcpoff' in generic_kern
for source in ('kern_start_generic.cpp','UVCLegacy0215.cpp','UVCRuntimeMode.cpp','UVCProbeControl.cpp',
               'UVCDescriptorParser.cpp','UVCProbeEngine.cpp','UVCRecoveryContext.cpp','UVCTransportTahoe.cpp','UVCQuirks.cpp'):
    assert f'\t\t\t\t\t{source},' in pbx,source
assert 'EXCLUDED_SOURCE_FILE_NAMES = (\n\t\t\t\t\tkern_start.cpp,' in pbx
assert 'UVCP_RELEASE_LEGACY_ONLY=1' not in pbx
assert 'GCC_ENABLE_CPP_EXCEPTIONS = NO;' in pbx
assert 'GCC_ENABLE_CPP_RTTI = NO;' in pbx
runtime_mode=(root/'UVCProbeFix'/'UVCRuntimeMode.cpp').read_text()
assert 'genericDefaultEnabled ? RuntimeMode::GenericFull : RuntimeMode::Legacy0215' in runtime_mode
assert (root/'Build-Test.command').is_file()
assert (root/'Tests'/'run_sanitizers.sh').is_file()
assert (root/'Tests'/'UVCTransportConcurrencyTests.cpp').is_file()
assert (root/'Tests'/'UVCSequoiaProbePatchTests.cpp').is_file()
for runner in ('run_host_tests.sh','run_sanitizers.sh'):
    assert 'UVCSequoiaProbePatchTests.cpp' in (root/'Tests'/runner).read_text(),runner
assert (root/'Tests'/'run_fuzz_smoke.sh').is_file()
assert (root/'Tests'/'Fuzz'/'fuzz_uvc_probe.cpp').is_file()
assert (root/'Tests'/'Fuzz'/'fuzz_uvc_descriptors.cpp').is_file()
assert (root/'Tests'/'Fuzz'/'fuzz_standalone_main.cpp').is_file()
assert (root/'Tests'/'Fixtures'/'fixture-manifest.json').is_file()
fuzz_script=(root/'Tests'/'run_fuzz_smoke.sh').read_text()
assert 'libfuzzer_probe.cpp' in fuzz_script
assert 'fuzz_standalone_main.cpp' in fuzz_script
assert 'UVC_FORCE_STANDALONE_FUZZ' in fuzz_script
assert '-runs=20000' in fuzz_script and '-max_len=64' in fuzz_script and '-max_len=4096' in fuzz_script
assert (root/'Tools'/'tahoe_kc_abi.py').is_file()
assert (root/'Tests'/'test_tahoe_kc_abi.py').is_file()

transport=(root/'UVCProbeFix'/'UVCTransportTahoe.cpp').read_text()
exact='__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj'
assert exact in transport
assert 'gOriginalDeviceRequest' in transport and 'routeMultiple' in transport
assert 'gCoordinator.isInternal' in transport
assert 'processCompletedGet' in transport
assert 'disableUnroutedTransport' in transport
assert 'IOLockFree(gContextLock)' in transport

# Generic decision code must contain no Lenovo identity or frozen repair values.
generic_paths=[]
for stem in ('UVCDescriptorParser','UVCProbeEngine','UVCRecoveryContext','UVCTransportTahoe'):
    generic_paths += [root/'UVCProbeFix'/f'{stem}.hpp',root/'UVCProbeFix'/f'{stem}.cpp']

def code_only(text):
    text=re.sub(r'/\*.*?\*/','',text,flags=re.S)
    text=re.sub(r'//[^\n]*','',text)
    text=re.sub(r'"(?:\\.|[^"\\])*"','""',text)
    return text

combined='\n'.join(code_only(x.read_text()) for x in generic_paths)
for forbidden in ('0x174F','0x1176','614400','2392','2400','3072'):
    assert forbidden not in combined,forbidden
assert not re.search(r'frameIndex\s*\(\s*\)\s*==\s*4',combined)
assert not re.search(r'setFrameIndex\s*\(\s*1\s*\)',combined)

quirks=(root/'UVCProbeFix'/'UVCQuirks.cpp').read_text()
assert 'return false;' in quirks

# Exact synchronous ABI selector is shared by source and target-side audit.
audit=(root/'Tools'/'Tahoe-IOUSBHost-ABI-Audit.command').read_text()
assert exact in audit
assert 'EXPECTED_SIGNATURE=' in audit
assert '25G229|25G241' in audit
assert 'BootKernelExtensions.kc' in audit and 'SystemKernelExtensions.kc' in audit
assert 'tahoe_kc_abi.py' in audit and 'TARGET_ENTRY="com.apple.iokit.IOUSBHostFamily"' in audit

build=(root/'Build.command').read_text()
assert 'Tahoe-IOUSBHost-ABI-Audit.command' in build
assert 'Tahoe-IOUSBHost-ABI.json' in build
assert 'if [ "$CONFIGURATION" = "Debug" ]; then' in build
assert 'Sequoia exact shared-cache path: skipping Tahoe generic IOUSBHost ABI gate' in build
assert 'case "$(/usr/bin/uname -r)" in' in build and '24.*)' in build
assert 'Release exact shared-cache build: skipping generic IOUSBHost ABI gate' in build
assert (root/'Tools'/'PostBoot-UVCProbeFix-Verify.command').is_file()
assert 'BUILD_NUMBER="303"' in build
assert '/usr/bin/codesign' in build
assert '--sign -' in build
assert 'codesign --verify' in build
assert build.count('Signature=adhoc') >= 2
assert 'Release executable unexpectedly contains generic IOUSBHost deviceRequest route' in build
assert 'Release Lenovo legacy source logic changed or HP module integration invalid' in build
assert 'test_release_entrypoint_isolation.py' in build
assert 'UVCProbeFix executable is missing integrated HP UVC 1.5 module' in build
assert 'Debug/Test executable is missing generic IOUSBHost deviceRequest route' in build
transport=(root/'UVCProbeFix'/'UVCTransportTahoe.cpp').read_text()
assert 'isSupportedTahoeDarwinVersion' in transport
assert 'getKernelVersion()' in transport and 'getKernelMinorVersion()' in transport

# No source binary contamination / CRLF in authored source and scripts.
for path in list((root/'UVCProbeFix').glob('*'))+list((root/'Tests').glob('*.cpp'))+list((root/'Tests'/'Fuzz').glob('*.cpp'))+list((root/'Tests').glob('*.sh'))+list((root/'Tests').glob('*.py'))+list((root/'Tools').glob('*.command'))+list((root/'Tools').glob('*.py')):
    if path.is_file():
        data=path.read_bytes()
        assert b'\x00' not in data,path
        assert b'\r\n' not in data,path

print('UVCProbeFix 0.3.3 Sequoia/Tahoe source invariants: PASS')
PY

while IFS= read -r -d '' script; do
    /bin/bash -n "$script"
done < <(find "$ROOT" -type f \( -name '*.command' -o -name '*.sh' \) \
    -not -path '*/.git/*' -not -path '*/.deps/*' -print0)
echo 'shell syntax: PASS'

"$PYTHON" "$ROOT/Tests/validate_macho.py" --self-test
"$PYTHON" "$ROOT/Tests/validate_release_zip.py" --self-test
"$PYTHON" "$ROOT/Tests/validate_fixtures.py"
"$ROOT/Tests/run_host_tests.sh"
echo "UVCProbeFix ${VERSION} source validation: PASS"
