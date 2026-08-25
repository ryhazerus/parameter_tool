#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

static int g_failures = 0;

static void Expect(const std::string& what, const std::string& got, const std::string& want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", what.c_str(), got.c_str(),
                     want.c_str());
        ++g_failures;
    }
}

int main() {
    // FIPS 180-4 / NIST published vectors.
    Expect("empty", pt::Sha256Hex(""),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Expect("abc", pt::Sha256Hex("abc"),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Expect("448-bit", pt::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Expect("896-bit",
           pt::Sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnop"
                         "jklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
           "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    // One million 'a' — exercises multi-block streaming and the 64-bit length field.
    {
        pt::Sha256 h;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.Update(chunk);
        Expect("1e6 x 'a'", h.FinalHex(),
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    // Awkward chunk sizes must agree with the one-shot hash.
    {
        const std::string data(5000, 'x');
        const std::string want = pt::Sha256Hex(data);
        for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64},
                                  std::size_t{65}, std::size_t{4096}}) {
            pt::Sha256 h;
            for (std::size_t off = 0; off < data.size(); off += chunk) {
                h.Update(data.data() + off, std::min(chunk, data.size() - off));
            }
            Expect("chunked " + std::to_string(chunk), h.FinalHex(), want);
        }
    }

    // Lengths straddling the 55/56/64 padding boundaries: byte-at-a-time must match one-shot.
    for (std::size_t len : {std::size_t{54}, std::size_t{55}, std::size_t{56}, std::size_t{57},
                            std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{119},
                            std::size_t{120}, std::size_t{128}}) {
        const std::string s(len, 'a');
        pt::Sha256 a;
        a.Update(s);
        const std::string one_shot = a.FinalHex();

        pt::Sha256 b;
        for (char c : s) b.Update(&c, 1);
        Expect("boundary len " + std::to_string(len), b.FinalHex(), one_shot);
    }

    if (g_failures == 0) std::printf("test_sha256: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
