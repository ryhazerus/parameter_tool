#include "scan.hpp"

#include <cstdio>
#include <string>

using pt::GlobMatch;

static int g_failures = 0;

static void Match(std::string_view pat, std::string_view name, bool want) {
    const bool got = GlobMatch(pat, name);
    if (got != want) {
        std::fprintf(stderr, "FAIL GlobMatch(\"%.*s\", \"%.*s\") = %s, want %s\n",
                     static_cast<int>(pat.size()), pat.data(), static_cast<int>(name.size()),
                     name.data(), got ? "true" : "false", want ? "true" : "false");
        ++g_failures;
    }
}

int main() {
    // The default pattern against realistic filenames.
    Match("parameter_*.xml", "parameter_a.xml", true);
    Match("parameter_*.xml", "parameter_motor_ctrl.xml", true);
    Match("parameter_*.xml", "parameter_.xml", true);   // empty <name> still matches
    Match("parameter_*.xml", "parameter_a.xml.bak", false);  // editor backup must be ignored
    Match("parameter_*.xml", "parameter_a.XML", false);      // case-sensitive
    Match("parameter_*.xml", "notes.txt", false);
    Match("parameter_*.xml", "parameter.xml", false);        // missing the underscore
    Match("parameter_*.xml", "old_parameter_a.xml", false);  // must match from the start
    Match("parameter_*.xml", "parameter_a.xml~", false);
    Match("parameter_*.xml", "xml", false);
    Match("parameter_*.xml", "", false);

    // Bare wildcards.
    Match("*", "anything", true);
    Match("*", "", true);
    Match("*.xml", ".xml", true);
    Match("?", "a", true);
    Match("?", "", false);
    Match("?", "ab", false);
    Match("a?c", "abc", true);
    Match("a?c", "ac", false);

    // Literal patterns.
    Match("exact.xml", "exact.xml", true);
    Match("exact.xml", "exact.xmls", false);
    Match("", "", true);
    Match("", "x", false);

    // Multiple and adjacent stars.
    Match("*a*", "bab", true);
    Match("*a*", "bbb", false);
    Match("**", "anything", true);
    Match("a**b", "ab", true);
    Match("a**b", "axxb", true);
    Match("*_*_*.xml", "parameter_motor_ctrl.xml", true);

    // The pathological backtracking case: must return promptly, not hang.
    Match("a*a*a*a*a*a*a*a*b", std::string(64, 'a'), false);
    Match("*a*a*a*a*a*a*a*a*", std::string(64, 'a'), true);

    // Trailing star absorbs the remainder, including nothing at all.
    Match("parameter_a*", "parameter_a", true);
    Match("parameter_a*", "parameter_ab", true);

    if (g_failures == 0) std::printf("test_glob: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
