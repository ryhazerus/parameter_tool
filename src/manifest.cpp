#include "manifest.hpp"

#include "error.hpp"
#include "json.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <ctime>

namespace pt {
namespace {

constexpr int kManifestVersion = 1;

[[noreturn]] void Bad(const std::string& origin, const std::string& what) {
    Fail("malformed snapshot manifest " + origin + ": " + what);
}

std::int64_t RequireInt(const json::Value& v, const char* key, const std::string& origin) {
    const auto* n = v.Find(key);
    if (!n) Bad(origin, std::string("missing key \"") + key + "\"");
    const auto i = n->AsInt();
    if (!i) Bad(origin, std::string("key \"") + key + "\" is not an integer");
    return *i;
}

std::string RequireStr(const json::Value& v, const char* key, const std::string& origin) {
    const auto* n = v.Find(key);
    if (!n) Bad(origin, std::string("missing key \"") + key + "\"");
    const auto* s = n->AsString();
    if (!s) Bad(origin, std::string("key \"") + key + "\" is not a string");
    return *s;
}

bool OptBool(const json::Value& v, const char* key, bool dflt) {
    const auto* n = v.Find(key);
    if (!n) return dflt;
    return n->AsBool().value_or(dflt);
}

std::string OptStr(const json::Value& v, const char* key, const std::string& dflt) {
    const auto* n = v.Find(key);
    if (!n) return dflt;
    const auto* s = n->AsString();
    return s ? *s : dflt;
}

}  // namespace

const FileEntry* Snapshot::Find(const std::string& p) const {
    // files is sorted by path, so a binary search is correct and cheap.
    auto it = std::lower_bound(files.begin(), files.end(), p,
                               [](const FileEntry& e, const std::string& k) { return e.path < k; });
    if (it != files.end() && it->path == p) return &*it;
    return nullptr;
}

std::uint64_t Snapshot::TotalSize() const {
    std::uint64_t t = 0;
    for (const auto& f : files) t += f.size;
    return t;
}

std::string SerializeSnapshot(const Snapshot& s) {
    json::Array files;
    files.reserve(s.files.size());
    for (const auto& f : s.files) {
        json::Object o;
        o.emplace_back("path", json::Value(f.path));
        o.emplace_back("hash", json::Value(f.hash));
        o.emplace_back("size", json::Value(static_cast<std::int64_t>(f.size)));
        o.emplace_back("mtime", json::Value(f.mtime));
        o.emplace_back("mode", json::Value(static_cast<std::int64_t>(f.mode)));
        files.push_back(json::Value(std::move(o)));
    }

    json::Object root;
    root.emplace_back("manifest_version", json::Value(kManifestVersion));
    root.emplace_back("id", json::Value(static_cast<std::int64_t>(s.id)));
    root.emplace_back("name", json::Value(s.name));
    root.emplace_back("created_utc", json::Value(s.created_utc));
    root.emplace_back("created_epoch", json::Value(s.created_epoch));
    root.emplace_back("scope", json::Value(s.scope == Scope::File ? "file" : "tree"));
    root.emplace_back("pattern", json::Value(s.pattern));
    root.emplace_back("recursive", json::Value(s.recursive));
    root.emplace_back("auto", json::Value(s.automatic));
    root.emplace_back("files", json::Value(std::move(files)));

    return json::Serialize(json::Value(std::move(root)));
}

Snapshot ParseSnapshot(const std::string& text, const std::string& origin) {
    std::string err;
    auto doc = json::Parse(text, &err);
    if (!doc) Bad(origin, err);
    if (!doc->AsObject()) Bad(origin, "top level is not an object");

    const std::int64_t ver = RequireInt(*doc, "manifest_version", origin);
    if (ver > kManifestVersion) {
        Fail("snapshot manifest " + origin + " was written by a newer parametertool (version " +
             std::to_string(ver) + "); upgrade the tool to read it");
    }

    Snapshot s;
    s.id = static_cast<int>(RequireInt(*doc, "id", origin));
    s.name = OptStr(*doc, "name", "");
    s.created_epoch = RequireInt(*doc, "created_epoch", origin);
    s.created_utc = OptStr(*doc, "created_utc", FormatUtc(s.created_epoch));

    const std::string scope = RequireStr(*doc, "scope", origin);
    if (scope == "tree") s.scope = Scope::Tree;
    else if (scope == "file") s.scope = Scope::File;
    else Bad(origin, "unknown scope \"" + scope + "\"");

    s.pattern = OptStr(*doc, "pattern", "*");
    s.recursive = OptBool(*doc, "recursive", true);
    s.automatic = OptBool(*doc, "auto", false);

    const auto* fn = doc->Find("files");
    if (!fn || !fn->AsArray()) Bad(origin, "missing or non-array \"files\"");
    for (const auto& fv : *fn->AsArray()) {
        if (!fv.AsObject()) Bad(origin, "entry in \"files\" is not an object");
        FileEntry e;
        e.path = RequireStr(fv, "path", origin);
        e.hash = RequireStr(fv, "hash", origin);
        if (e.hash.size() != 64) Bad(origin, "entry \"" + e.path + "\" has a malformed hash");
        e.size = static_cast<std::uint64_t>(RequireInt(fv, "size", origin));
        e.mtime = RequireInt(fv, "mtime", origin);
        e.mode = static_cast<std::uint32_t>(RequireInt(fv, "mode", origin));
        s.files.push_back(std::move(e));
    }

    // Find() binary-searches, so re-establish the invariant even if the file was hand-edited.
    std::sort(s.files.begin(), s.files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    return s;
}

std::string FormatUtc(std::int64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string FormatSize(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[u]);
    return buf;
}

}  // namespace pt
