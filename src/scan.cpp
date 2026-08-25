#include "scan.hpp"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {

bool GlobMatch(std::string_view pattern, std::string_view name) {
    // Iterative backtracking: remember the last '*' and resume there on mismatch. Linear in the
    // common case and immune to the exponential blowup a naive recursive matcher hits on
    // patterns like "a*a*a*a*b".
    std::size_t p = 0, n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t match = 0;

    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string ToRelPosix(const fs::path& p) { return p.generic_string(); }

ScanResult ScanTree(const fs::path& root, std::string_view pattern, bool recursive) {
    ScanResult out;

    const auto consider = [&](const fs::directory_entry& e) {
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') return;  // dotfiles are never config parameters

        std::error_code ec;
        if (e.is_symlink(ec)) {
            out.warnings.push_back("skipped symlink: " + ToRelPosix(fs::relative(e.path(), root, ec)));
            return;
        }
        if (!e.is_regular_file(ec) || ec) return;
        if (!GlobMatch(pattern, name)) return;

        const fs::path rel = fs::relative(e.path(), root, ec);
        if (ec) {
            out.warnings.push_back("cannot relativize: " + e.path().string());
            return;
        }
        out.files.push_back(ToRelPosix(rel));
    };

    std::error_code ec;
    if (recursive) {
        auto it = fs::recursive_directory_iterator(
            root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            out.warnings.push_back("cannot read directory " + root.string() + ": " + ec.message());
            return out;
        }
        for (auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
            if (ec) {
                out.warnings.push_back("error while walking: " + ec.message());
                ec.clear();
                continue;
            }
            const std::string name = it->path().filename().string();

            std::error_code dec;
            // Never descend into the store or any other dot-directory (.git and friends).
            if (it->is_directory(dec) && !dec && !name.empty() && name[0] == '.') {
                it.disable_recursion_pending();
                continue;
            }
            // Don't follow directory symlinks either.
            if (it->is_symlink(dec) && !dec && it->is_directory(dec)) {
                it.disable_recursion_pending();
            }
            consider(*it);
        }
    } else {
        auto it = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            out.warnings.push_back("cannot read directory " + root.string() + ": " + ec.message());
            return out;
        }
        for (const auto& e : it) consider(e);
    }

    std::sort(out.files.begin(), out.files.end());
    return out;
}

}  // namespace pt
