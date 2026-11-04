#!/usr/bin/env python3
import hashlib
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
release_entry = root / 'UVCProbeFix' / 'kern_start.cpp'
generic_entry = root / 'UVCProbeFix' / 'kern_start_generic.cpp'
pbx_path = root / 'UVCProbeFix.xcodeproj' / 'project.pbxproj'

KNOWN_0215_KERN_START_SHA256 = 'e5788e796f55dbc891484c934aff64983fe510b973b6ba8fe81b81f6e1484b74'
integrated = release_entry.read_text()
assert integrated.count('#include "UVC15ProbeOverrun.hpp" // HP_INTEGRATION_INCLUDE') == 1
assert integrated.count('// HP_INTEGRATION_BEGIN') == 1
assert integrated.count('// HP_INTEGRATION_END') == 1
assert integrated.count('#include "UVCSequoia.hpp" // SEQUOIA_INTEGRATION_INCLUDE') == 1
assert integrated.count('// SEQUOIA_INTEGRATION_BEGIN') == 1
assert integrated.count('// SEQUOIA_INTEGRATION_END') == 1
restored = integrated.replace('\n#include "UVC15ProbeOverrun.hpp" // HP_INTEGRATION_INCLUDE', '', 1)
restored = re.sub(r'    // HP_INTEGRATION_BEGIN[^\n]*\n.*?    // HP_INTEGRATION_END\n', '', restored, count=1, flags=re.S)
restored = restored.replace('\n#include "UVCSequoia.hpp" // SEQUOIA_INTEGRATION_INCLUDE', '', 1)
restored = re.sub(r'    // SEQUOIA_INTEGRATION_BEGIN[^\n]*\n.*?    // SEQUOIA_INTEGRATION_END\n', '', restored, count=1, flags=re.S)
supported_range = '    KernelVersion::Sequoia,\n    KernelVersion::Tahoe,\n'
assert restored.count(supported_range) == 1
restored = restored.replace(supported_range, '    KernelVersion::Tahoe,\n    KernelVersion::Tahoe,\n', 1)
major_only_gate = '    if (getKernelVersion() != KernelVersion::Tahoe) {\n'
original_gate = ('    if (getKernelVersion() != KernelVersion::Tahoe ||\n'
                 '        getKernelMinorVersion() != UVCProbePatch::kDarwinMinor) {\n')
assert restored.count(major_only_gate) == 1
assert 'getKernelMinorVersion() != UVCProbePatch::kDarwinMinor' not in restored
restored = restored.replace(major_only_gate, original_gate, 1)
major_gate_comment = ('    // OpenCore admits Darwin 24.x/25.x; this path handles every Tahoe minor.\n'
                      '    // Exact page bytes remain the stronger build-family gate.\n')
original_gate_comment = ('    // OpenCore independently restricts this kext to Darwin 25.6.0. Keep a\n'
                         '    // runtime guard as well; exact page bytes provide the stronger build-family gate.\n')
assert restored.count(major_gate_comment) == 1
restored = restored.replace(major_gate_comment, original_gate_comment, 1)
major_gate_message = 'unsupported Darwin major version for known Tahoe UVCFamily fingerprints; plugin is inactive'
original_gate_message = 'unsupported Darwin version for known Tahoe 25.6 UVCFamily fingerprints; plugin is inactive'
assert restored.count(major_gate_message) == 1
restored = restored.replace(major_gate_message, original_gate_message, 1)
actual = hashlib.sha256(restored.encode()).hexdigest()
assert actual == KNOWN_0215_KERN_START_SHA256, (actual, KNOWN_0215_KERN_START_SHA256)
assert 'UVC15::start()' in integrated
assert 'UVCSequoia::start()' in integrated
assert 'UVC15ProbeOverrun.cpp in Sources' in pbx_path.read_text()
assert generic_entry.is_file(), 'generic entrypoint must be separate from Release kern_start.cpp'
generic = generic_entry.read_text()
assert 'UVC::startTahoeTransport(mode)' in generic
assert 'UVCLegacy0215::start()' in generic
assert 'UVCSequoia::start()' in generic
assert supported_range in generic
legacy = (root / 'UVCProbeFix' / 'UVCLegacy0215.cpp').read_text()
assert legacy.count(major_only_gate) == 1
assert 'getKernelMinorVersion() != UVCProbePatch::kDarwinMinor' not in legacy
assert major_gate_comment in legacy and major_gate_message in legacy

pbx = pbx_path.read_text()
# Every PBXBuildFile fileRef used by a source must have a PBXFileReference declaration.
used = set(re.findall(r'fileRef = (A2[0-9A-F]{22})', pbx))
declared = set(re.findall(r'^\s*(A2[0-9A-F]{22}) /\* .*? \*/ = \{isa = PBXFileReference;', pbx, re.M))
missing = sorted(used - declared)
assert not missing, f'PBXBuildFile references undeclared PBXFileReference IDs: {missing}'

for name in ('kern_start.cpp', 'kern_start_generic.cpp', 'UVCLegacy0215.cpp', 'UVCRuntimeMode.cpp',
             'UVCProbeControl.cpp', 'UVCDescriptorParser.cpp', 'UVCProbeEngine.cpp',
             'UVCRecoveryContext.cpp', 'UVCTransportTahoe.cpp', 'UVCQuirks.cpp', 'UVCSequoia.cpp'):
    assert f'{name} in Sources' in pbx, name

# Production Release preserves the exact 0.2.15 Tahoe body and adds the isolated
# Sequoia path; generic/dispatcher sources remain Debug/Test-only.
debug_block = re.search(r'AA0000000000000000000003 /\* Debug \*/ = \{.*?\n\s*\};\n\s*name = Debug;', pbx, re.S)
release_block = re.search(r'AA0000000000000000000004 /\* Release \*/ = \{.*?\n\s*\};\n\s*name = Release;', pbx, re.S)
assert debug_block and release_block
dbg = debug_block.group(0)
rel = release_block.group(0)
assert 'EXCLUDED_SOURCE_FILE_NAMES' in dbg and 'kern_start.cpp' in dbg
for name in ('kern_start_generic.cpp','UVCLegacy0215.cpp','UVCRuntimeMode.cpp','UVCProbeControl.cpp',
             'UVCDescriptorParser.cpp','UVCProbeEngine.cpp','UVCRecoveryContext.cpp','UVCTransportTahoe.cpp','UVCQuirks.cpp'):
    assert name in rel, f'Release must exclude {name}'
assert 'kern_start.cpp' not in rel.split('EXCLUDED_SOURCE_FILE_NAMES',1)[1].split(');',1)[0]
assert 'UVC15ProbeOverrun.cpp' not in rel.split('EXCLUDED_SOURCE_FILE_NAMES',1)[1].split(');',1)[0]
assert 'UVCSequoia.cpp' not in rel.split('EXCLUDED_SOURCE_FILE_NAMES',1)[1].split(');',1)[0]
assert 'UVCSequoia.cpp' not in dbg.split('EXCLUDED_SOURCE_FILE_NAMES',1)[1].split(');',1)[0]
for name in ('UVCSequoia.hpp', 'UVCSequoiaProbePatch.hpp'):
    assert f'path = {name};' in pbx, name
print('test_release_entrypoint_isolation: PASS')
