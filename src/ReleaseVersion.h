// Release tags use two or three numeric components and optional prerelease IDs.
// No network or platform dependencies; only public release tags are accepted.
#pragma once
#include <array>
#include <charconv>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace release_updates {
inline bool numeric(std::string_view text) {
    return !text.empty() && text.find_first_not_of("0123456789") == text.npos;
}
inline std::strong_ordering compare_identifiers(std::string_view left, std::string_view right) {
    while (!left.empty() && !right.empty()) {
        const auto a = left.substr(0, left.find('.')), b = right.substr(0, right.find('.'));
        if (a != b) {
            const bool an = numeric(a), bn = numeric(b);
            if (an != bn) return an ? std::strong_ordering::less : std::strong_ordering::greater;
            if (an) {
                auto x = a, y = b;
                while (x.size() > 1 && x.front() == '0') x.remove_prefix(1);
                while (y.size() > 1 && y.front() == '0') y.remove_prefix(1);
                if (x.size() != y.size()) return x.size() <=> y.size();
                if (x != y) return x <=> y;
            } else return a <=> b;
        }
        left = left.size() == a.size() ? std::string_view{} : left.substr(a.size() + 1);
        right = right.size() == b.size() ? std::string_view{} : right.substr(b.size() + 1);
    }
    if (left.empty() == right.empty()) return std::strong_ordering::equal;
    return left.empty() ? std::strong_ordering::less : std::strong_ordering::greater;
}
struct Version {
    uint32_t major{}, minor{}, patch{};
    std::string prerelease;
    std::strong_ordering operator<=>(const Version& other) const {
        const auto numbers = std::array{major, minor, patch} <=> std::array{other.major, other.minor, other.patch};
        if (numbers != 0) return numbers;
        if (prerelease.empty() != other.prerelease.empty())
            return prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
        return compare_identifiers(prerelease, other.prerelease);
    }
    bool operator==(const Version& other) const { return (*this <=> other) == 0; }
};
inline std::optional<Version> parse(std::string_view text) {
    if (text.starts_with("dlss5-video-player-")) text.remove_prefix(19);
    if (text.starts_with('v') || text.starts_with('V')) text.remove_prefix(1);
    if (text.empty() || text.size() > 64 || text.find('+') != text.npos) return {};
    Version version;
    const auto dash = text.find('-');
    if (dash != text.npos) {
        const auto suffix = text.substr(dash + 1);
        if (suffix.empty() || suffix.front() == '.' || suffix.back() == '.' || suffix.find("..") != suffix.npos ||
            suffix.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-") != suffix.npos) return {};
        version.prerelease = suffix;
        text = text.substr(0, dash);
    }
    uint32_t* values[]{&version.major, &version.minor, &version.patch};
    unsigned count = 0;
    while (!text.empty() && count < 3) {
        const auto dot = text.find('.'); const auto part = text.substr(0, dot);
        if (!numeric(part)) return {};
        const auto result = std::from_chars(part.data(), part.data() + part.size(), *values[count++]);
        if (result.ec != std::errc{}) return {};
        if (dot == text.npos) { text = {}; break; }
        text.remove_prefix(dot + 1);
        if (text.empty()) return {};
    }
    if (!text.empty() || count < 2) return {};
    return version;
}
} // namespace release_updates
