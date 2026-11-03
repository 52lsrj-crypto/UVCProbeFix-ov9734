// Copyright © 2026 DNMTechLabs. All rights reserved.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#if __has_include("../UVCProbeFix/UVCSequoiaProbePatch.hpp")
#include "../UVCProbeFix/UVCSequoiaProbePatch.hpp"

#if defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace UVCSequoiaProbePatch;

std::vector<uint8_t> readFixture() {
    const std::string source = __FILE__;
    const auto slash = source.find_last_of("/\\");
    std::ifstream f(source.substr(0, slash + 1) +
        "Fixtures/Sequoia-24A335-doProbeControl.bin", std::ios::binary);
    assert(f.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void testExactCapturedMethodAndPageMatch() {
    const auto fixture = readFixture();
    assert(fixture.size() == 0x454);
    assert(sizeof(kOriginalMethod) == fixture.size());
    assert(std::memcmp(fixture.data(), kOriginalMethod, fixture.size()) == 0);
    std::array<uint8_t, 4096> page {};
    std::memcpy(page.data() + 0x3BB, fixture.data(), fixture.size());
    std::size_t offset = 999;
    assert(findUniqueMethod(page.data(), page.size(), offset));
    assert(offset == 0x3BB);
    assert(!findUniqueMethod(nullptr, page.size(), offset));
    assert(!findUniqueMethod(page.data(), fixture.size() - 1, offset));

    // Every byte belongs to the fingerprint, including stack and retry code.
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        page[0x3BB + i] ^= 1;
        assert(!findUniqueMethod(page.data(), page.size(), offset));
        page[0x3BB + i] ^= 1;
    }
    std::memcpy(page.data() + 0x900, fixture.data(), fixture.size());
    assert(!findUniqueMethod(page.data(), page.size(), offset));
    page.fill(0);
    std::memcpy(page.data() + page.size() - fixture.size(), fixture.data(), fixture.size());
    assert(findUniqueMethod(page.data(), page.size(), offset));
    assert(offset == page.size() - fixture.size());
}

void testReplacementPreservesAllOtherBytes() {
    const auto fixture = readFixture();
    std::array<uint8_t, 0x454> patched {};
    assert(buildReplacementMethod(patched.data(), patched.size()));
    assert(!buildReplacementMethod(nullptr, patched.size()));
    auto unchanged = patched;
    assert(!buildReplacementMethod(patched.data(), patched.size() - 1));
    assert(patched == unchanged);
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        if (i < 0x302 || i >= 0x345) assert(patched[i] == fixture[i]);
    }
    assert(isSupportedDarwinVersion(24, 0));
    assert(!isSupportedDarwinVersion(24, 1));
    assert(!isSupportedDarwinVersion(23, 0));
    assert(!isSupportedDarwinVersion(25, 0));
}

#if defined(__x86_64__)
// Only the injected block executes. Apple's request and continuation endpoints
// are instrumented stubs; no operating-system method or USB request executes.
struct NativeResult {
    uint64_t retries = 0;
    uint64_t retryR15 = 0;
    uint64_t retryR12 = 0;
    uint64_t reloadedError = 0;
    uint64_t resumeR15 = 0;
    uint64_t resumeR12 = 0;
    uint64_t repeatMalformed = 0;
    uint64_t initialR12 = 0x31;
    uint64_t initialR15 = 0x11223344;
};
static_assert(offsetof(NativeResult, initialR15) == 64, "assembly layout");
extern "C" void invokeSequoiaPatch(const void *, uint8_t *, uint8_t *, NativeResult *);
#if defined(__APPLE__)
#define ASM_NAME "_invokeSequoiaPatch"
#else
#define ASM_NAME "invokeSequoiaPatch"
#endif
__asm__(".text\n.globl " ASM_NAME "\n" ASM_NAME ":\n"
        "pushq %rbp\n pushq %rbx\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
        "movq %rsp,%rbx\n movq %rcx,%r14\n movq %rsi,%r13\n"
        "leaq 0x90(%rdx),%rbp\n movq %rdx,%rsp\n"
        "movq 56(%r14),%r12\n movq 64(%r14),%r15\n callq *%rdi\n"
        "movq %rbx,%rsp\n popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbx\n popq %rbp\n retq\n");
#undef ASM_NAME

void write32(uint8_t *p, uint32_t x) { std::memcpy(p, &x, 4); }
uint32_t read32(const uint8_t *p) { uint32_t x; std::memcpy(&x, p, 4); return x; }
std::array<uint8_t, 26> malformed() {
    std::array<uint8_t, 26> p {};
    p[0] = 1; p[2] = 1; p[3] = 4;
    write32(p.data() + 4, 333333);
    write32(p.data() + 18, 0x0B000000);
    write32(p.data() + 22, 0x01010001);
    return p;
}

class NativeHarness {
    uint8_t *code = nullptr;
    std::size_t bytes = 0;
public:
    NativeHarness() {
        bytes = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        assert(bytes >= 4096);
        code = static_cast<uint8_t *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANON, -1, 0));
        assert(code != MAP_FAILED);
        std::memset(code, 0xCC, bytes);
        std::array<uint8_t, kMethodSize> method {};
        assert(buildReplacementMethod(method.data(), method.size()));
        std::memcpy(code + 0x302, method.data() + 0x302, 67);
        // Retry endpoint jumps to an instrumented stub at method +0x800.
        code[0x21A] = 0xE9;
        write32(code + 0x21B, 0x800 - 0x21F);
        const uint8_t resume[] {0x4D,0x89,0x7E,0x20,0x4D,0x89,0x66,0x28,0xC3};
        std::memcpy(code + 0x384, resume, sizeof(resume));
        const uint8_t retry[] {
            0x49,0xFF,0x06,                    // ++retries
            0x49,0x83,0x3E,0x02,              // bound a broken-patch test to two retries
            0x0F,0x83,0,0,0,0,
            0x4D,0x89,0x7E,0x08,              // R15 at retry boundary
            0x4D,0x89,0x66,0x10,              // R12 at retry boundary
            0x4C,0x8B,0x65,0x90,              // captured original +0x238
            0x4D,0x89,0x66,0x18,              // record reloaded error:
            0x49,0x83,0x7E,0x30,0x00,         // repeat malformed response?
            0x74,0x1A,
            0x41,0xC6,0x45,0x03,0x04,
            0x41,0xC7,0x45,0x12,0,0,0,0x0B,
            0x41,0xC7,0x45,0x16,1,0,1,1,
            0xE9,0,0,0,0,                    // re-enter post-GET block
            0xE9,0,0,0,0                     // continue after first retry
        };
        std::memcpy(code + 0x800, retry, sizeof(retry));
        // Derive branch displacements from the stub's independently fixed sites.
        write32(code + 0x809, static_cast<uint32_t>(0x384 - 0x80D));
        std::memcpy(code + 0x815, kOriginalMethod + 0x238, 4);
        write32(code + 0x83A, static_cast<uint32_t>(0x302 - 0x83E));
        write32(code + 0x83F, static_cast<uint32_t>(0x384 - 0x843));
        assert(mprotect(code, bytes, PROT_READ | PROT_EXEC) == 0);
    }
    ~NativeHarness() { assert(munmap(code, bytes) == 0); }
    void run(std::array<uint8_t, 26> &probe, NativeResult &r,
             uint8_t initialFlag = 0, uint8_t expectedFlag = 0) {
        alignas(16) std::array<uint8_t, 512> stack;
        stack.fill(0xA5);
        uint8_t *base = stack.data() + 0x80;
        const uint64_t frameRate = 333333, object = 0x13579BDF2468ACE0,
                       error = 0x1234567812345678;
        std::memcpy(base + 8, &frameRate, 8);      // rbp-0x88
        std::memcpy(base + 0x20, &error, 8);      // rbp-0x70
        std::memcpy(base + 0x28, &object, 8);     // rbp-0x68
        base[0x0C] = initialFlag;
        const auto before = stack;
        invokeSequoiaPatch(code + 0x302, probe.data(), base, &r);
        for (std::size_t i = 0; i < stack.size(); ++i) {
            if (i >= 0x78 && i < 0x80) continue; // trampoline return address
            if (i == 0x8C) continue;            // documented dead scratch byte
            assert(stack[i] == before[i]);
        }
        assert(read32(base + 8) == 333333);      // live interval is untouched
        assert(base[0x0C] == expectedFlag);
        if (r.retries) {
            assert(r.retryR15 == object);
            assert(r.retryR12 == r.initialR12);
            assert(r.reloadedError == error);
        }
    }
};

void testMachineCodePassThroughAndRetry() {
    NativeHarness h;
    auto p = malformed();
    NativeResult result;
    h.run(p, result, 0, 1);
    assert(result.retries == 1);
    assert(p[2] == 1 && p[3] == 1 && read32(p.data() + 4) == 333333);
    assert(read32(p.data() + 18) == 614400 && read32(p.data() + 22) == 0);

    p = malformed(); result = {}; result.repeatMalformed = 1;
    h.run(p, result);
    assert(result.retries == 1);                 // malformed second GET cannot loop
    assert(p[3] == 1 && read32(p.data() + 22) == 0);

    p = malformed(); result = {};
    h.run(p, result, 1);
    assert(result.retries == 0);                 // already retried on entry
    assert(result.resumeR12 == result.initialR12);
    assert(result.resumeR15 == result.initialR15);

    p = malformed(); p[3] = 1;
    write32(p.data() + 18, 614400); write32(p.data() + 22, 3072);
    const auto knownGood = p;
    result = {};
    h.run(p, result);
    assert(p == knownGood && result.retries == 0);

    // Every compared byte independently rejects a near miss; these include
    // format/frame/interval, reserved tuple bytes, frameSize and payload.
    for (const unsigned i : {2U,3U,4U,5U,6U,7U,8U,9U,18U,19U,20U,21U,22U,23U,24U,25U}) {
        p = malformed(); p[i] ^= 1;
        const auto before = p;
        result = {};
        h.run(p, result);
        assert(result.retries == 0 && p == before);
        assert(result.resumeR12 == result.initialR12);
        assert(result.resumeR15 == result.initialR15);
    }
}
#endif
} // namespace

int main() {
    testExactCapturedMethodAndPageMatch();
    testReplacementPreservesAllOtherBytes();
#if defined(__x86_64__)
    testMachineCodePassThroughAndRetry();
    std::cout << "Sequoia24A335 exact match, byte preservation and native one-shot retry: PASS\n";
#else
    std::cout << "Sequoia24A335 exact match and byte preservation: PASS (native x86_64 execution skipped)\n";
#endif
}
#else
int main() { assert(false && "Sequoia exact-method adaptation is not implemented"); }
#endif
