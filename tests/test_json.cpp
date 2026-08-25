#include "json.hpp"

#include <cstdio>
#include <string>

using namespace pt;

static int g_failures = 0;

static void Check(const std::string& what, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

static void ExpectEq(const std::string& what, const std::string& got, const std::string& want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got  [%s]\n  want [%s]\n", what.c_str(), got.c_str(),
                     want.c_str());
        ++g_failures;
    }
}

// Round-trips a string through a manifest-shaped document and returns what came back out.
static std::string RoundTripName(const std::string& name) {
    json::Object o;
    o.emplace_back("name", json::Value(name));
    const std::string text = json::Serialize(json::Value(std::move(o)));

    std::string err;
    auto parsed = json::Parse(text, &err);
    if (!parsed) {
        std::fprintf(stderr, "  parse error: %s\n  text: %s\n", err.c_str(), text.c_str());
        return "<parse-failed>";
    }
    const auto* v = parsed->Find("name");
    if (!v || !v->AsString()) return "<missing>";
    return *v->AsString();
}

int main() {
    // Snapshot names are user-supplied, so escaping has to survive anything they type.
    for (const std::string& s : {
             std::string("version 1"),
             std::string("quote\"inside"),
             std::string("back\\slash"),
             std::string("tab\there"),
             std::string("newline\nhere"),
             std::string("ctrl\x01\x1f"),
             std::string("unicode: \xc3\xa9\xe2\x82\xac \xf0\x9f\x9a\x80"),
             std::string("slash/forward"),
             std::string(""),
         }) {
        ExpectEq("roundtrip [" + json::EscapeString(s) + "]", RoundTripName(s), s);
    }

    // A full manifest-shaped document.
    {
        json::Array files;
        {
            json::Object f;
            f.emplace_back("path", json::Value("parameter_a.xml"));
            f.emplace_back("hash", json::Value("ab12"));
            f.emplace_back("size", json::Value(std::int64_t{2048}));
            f.emplace_back("mtime", json::Value(std::int64_t{1787654000}));
            f.emplace_back("mode", json::Value(std::int64_t{420}));
            files.push_back(json::Value(std::move(f)));
        }
        json::Object root;
        root.emplace_back("id", json::Value(std::int64_t{7}));
        root.emplace_back("name", json::Value("version 1"));
        root.emplace_back("auto", json::Value(false));
        root.emplace_back("recursive", json::Value(true));
        root.emplace_back("files", json::Value(std::move(files)));

        const std::string text = json::Serialize(json::Value(std::move(root)));
        std::string err;
        auto p = json::Parse(text, &err);
        Check("manifest parses", p.has_value());
        if (p) {
            Check("id == 7", p->Find("id") && p->Find("id")->AsInt() == 7);
            Check("auto == false", p->Find("auto") && p->Find("auto")->AsBool() == false);
            Check("recursive == true", p->Find("recursive") && p->Find("recursive")->AsBool() == true);
            const auto* fs = p->Find("files") ? p->Find("files")->AsArray() : nullptr;
            Check("one file", fs && fs->size() == 1);
            if (fs && fs->size() == 1) {
                Check("size == 2048", (*fs)[0].Find("size")->AsInt() == 2048);
                Check("path", *(*fs)[0].Find("path")->AsString() == "parameter_a.xml");
            }
        }
    }

    // Type confusion must be reported, not silently coerced.
    {
        std::string err;
        auto p = json::Parse(R"({"id": "seven"})", &err);
        Check("string id parses", p.has_value());
        Check("AsInt on string is nullopt", p && !p->Find("id")->AsInt().has_value());
        Check("missing key is nullptr", p && p->Find("nope") == nullptr);
    }

    // Escapes on the way in.
    {
        std::string err;
        auto p = json::Parse(R"({"s":"aAb\n\t\"\\\/ é 😀"})", &err);
        Check("escape doc parses", p.has_value());
        if (p) ExpectEq("decoded escapes", *p->Find("s")->AsString(),
                        std::string("aAb\n\t\"\\/ \xc3\xa9 \xf0\x9f\x98\x80"));
    }

    // Negative numbers and empty containers.
    {
        std::string err;
        auto p = json::Parse(R"({"n":-42,"z":0,"a":[],"o":{}})", &err);
        Check("numeric doc parses", p.has_value());
        if (p) {
            Check("negative", p->Find("n")->AsInt() == -42);
            Check("zero", p->Find("z")->AsInt() == 0);
            Check("empty array", p->Find("a")->AsArray() && p->Find("a")->AsArray()->empty());
            Check("empty object", p->Find("o")->AsObject() && p->Find("o")->AsObject()->empty());
        }
    }

    // Malformed input must fail rather than half-parse — a corrupt manifest should be loud.
    for (const char* bad : {
             "{",  "}", "[", R"({"a")", R"({"a":})", R"({"a":1,})", R"([1,])",
             R"({"a":01})", R"({"a":1} trailing)", R"({"a":"unterminated)",
             R"({"a":tru})", R"({a:1})", R"({"a":1.})", R"({"a":1e})", "",
             R"({"a":"raw
newline"})",
         }) {
        std::string err;
        auto p = json::Parse(bad, &err);
        Check(std::string("rejects [") + bad + "]", !p.has_value() && !err.empty());
    }

    if (g_failures == 0) std::printf("test_json: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
