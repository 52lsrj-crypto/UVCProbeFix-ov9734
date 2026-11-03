#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace {

struct Options {
    uint64_t seed {2303U};
    size_t runs {20000U};
    size_t maxLen {4096U};
    std::string corpusDir;
};

uint64_t parseUnsigned(const char *text, const char *name) {
    if (!text || !*text) {
        std::fprintf(stderr, "invalid %s\n", name);
        std::exit(2);
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        std::fprintf(stderr, "invalid %s: %s\n", name, text);
        std::exit(2);
    }
    return static_cast<uint64_t>(value);
}

Options parseOptions(int argc, char **argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strncmp(arg, "-seed=", 6) == 0) {
            out.seed = parseUnsigned(arg + 6, "seed");
        } else if (std::strncmp(arg, "-runs=", 6) == 0) {
            out.runs = static_cast<size_t>(parseUnsigned(arg + 6, "runs"));
        } else if (std::strncmp(arg, "-max_len=", 9) == 0) {
            out.maxLen = static_cast<size_t>(parseUnsigned(arg + 9, "max_len"));
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "unsupported standalone fuzz option: %s\n", arg);
            std::exit(2);
        } else if (out.corpusDir.empty()) {
            out.corpusDir = arg;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", arg);
            std::exit(2);
        }
    }
    if (out.maxLen > 4096U) {
        std::fprintf(stderr, "max_len exceeds bounded limit: %zu\n", out.maxLen);
        std::exit(2);
    }
    return out;
}

class DeterministicRng {
public:
    explicit DeterministicRng(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    uint64_t next() {
        uint64_t x = state;
        x ^= x >> 12U;
        x ^= x << 25U;
        x ^= x >> 27U;
        state = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    size_t bounded(size_t limit) {
        return limit == 0U ? 0U : static_cast<size_t>(next() % limit);
    }

private:
    uint64_t state;
};

std::vector<std::vector<uint8_t>> loadCorpus(const std::string &dir, size_t maxLen) {
    std::vector<std::vector<uint8_t>> corpus;
    if (dir.empty()) return corpus;

    DIR *handle = opendir(dir.c_str());
    if (!handle) {
        std::perror("opendir corpus");
        std::exit(2);
    }

    constexpr size_t kMaxCorpusFiles = 256U;
    while (corpus.size() < kMaxCorpusFiles) {
        dirent *entry = readdir(handle);
        if (!entry) break;
        if (entry->d_name[0] == '.') continue;

        const std::string path = dir + "/" + entry->d_name;
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;

        std::vector<uint8_t> bytes;
        bytes.reserve(maxLen);
        char ch = 0;
        while (bytes.size() < maxLen && input.get(ch)) {
            bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(ch)));
        }
        corpus.push_back(bytes);
    }
    closedir(handle);
    return corpus;
}

void invoke(const std::vector<uint8_t> &bytes) {
    static const uint8_t zero = 0U;
    const uint8_t *data = bytes.empty() ? &zero : bytes.data();
    (void)LLVMFuzzerTestOneInput(data, bytes.size());
}

std::vector<uint8_t> makeRandom(DeterministicRng &rng, size_t maxLen) {
    const size_t size = rng.bounded(maxLen + 1U);
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(rng.next());
    }
    return bytes;
}

void mutate(std::vector<uint8_t> &bytes, DeterministicRng &rng, size_t maxLen) {
    const size_t mutations = 1U + rng.bounded(8U);
    for (size_t n = 0; n < mutations; ++n) {
        switch (rng.bounded(4U)) {
            case 0U:
                if (!bytes.empty()) bytes[rng.bounded(bytes.size())] ^= static_cast<uint8_t>(1U << rng.bounded(8U));
                break;
            case 1U:
                if (!bytes.empty()) bytes[rng.bounded(bytes.size())] = static_cast<uint8_t>(rng.next());
                break;
            case 2U:
                if (bytes.size() < maxLen) bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(rng.bounded(bytes.size() + 1U)), static_cast<uint8_t>(rng.next()));
                break;
            case 3U:
                if (!bytes.empty()) bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(rng.bounded(bytes.size())));
                break;
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    const Options options = parseOptions(argc, argv);
    const auto corpus = loadCorpus(options.corpusDir, options.maxLen);
    DeterministicRng rng(options.seed);

    size_t executed = 0U;
    for (const auto &seed : corpus) {
        if (executed >= options.runs) break;
        invoke(seed);
        ++executed;
    }

    while (executed < options.runs) {
        std::vector<uint8_t> input;
        if (!corpus.empty() && (rng.next() & 1U) != 0U) {
            input = corpus[rng.bounded(corpus.size())];
            mutate(input, rng, options.maxLen);
        } else {
            input = makeRandom(rng, options.maxLen);
        }
        invoke(input);
        ++executed;
    }

    std::printf("standalone sanitizer fuzz: %zu runs, max_len=%zu, corpus=%zu: PASS\n",
                executed, options.maxLen, corpus.size());
    return 0;
}
