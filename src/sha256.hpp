#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pt {

// FIPS 180-4 SHA-256. Streaming: construct, Update() any number of times, Final() once.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;
    using Digest = std::array<std::uint8_t, kDigestBytes>;

    Sha256() = default;

    void Update(const void* data, std::size_t len);
    void Update(std::string_view s) { Update(s.data(), s.size()); }

    // Finalizes the hash. The object must not be Updated afterwards.
    Digest Final();

    // Lowercase hex of Final().
    std::string FinalHex();

private:
    void Compress(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

std::string ToHex(const Sha256::Digest& d);

// Convenience: hash a contiguous buffer in one shot.
std::string Sha256Hex(std::string_view data);

}  // namespace pt
