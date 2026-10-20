#include "TestFramework.h"

#include "core/Pattern.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

// Phase 2 final review (docs): the user-facing texts that no code renders (README, the installer's Quick Start,
// ARCHITECTURE, the README screenshot) must not fall behind the plugin again. CB_SOURCE_DIR is the checkout root
// (set by CMakeLists.txt for CoreTests only).

#ifndef CB_SOURCE_DIR
 #error "CB_SOURCE_DIR must point at the source checkout"
#endif

using namespace cb;

namespace
{
std::string readRepoFile (const char* relative)
{
    std::ifstream in (std::string (CB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    return std::string (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
}

bool contains (const std::string& text, const std::string& what) { return text.find (what) != std::string::npos; }

// Markdown / text files are hard-wrapped: compare with every run of whitespace folded to one space.
std::string folded (const std::string& text)
{
    std::string out;
    bool space = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
        {
            space = true;
            continue;
        }
        if (space && ! out.empty())
            out += ' ';
        space = false;
        out += c;
    }
    return out;
}
} // namespace

TEST_CASE ("docs: installer Quick Start names the five export chips and the Roll (no Both chip)")
{
    const auto raw = readRepoFile ("installer/QuickStart.txt");
    REQUIRE (! raw.empty());
    const auto text = folded (raw);

    CHECK (! contains (text, "\"Both\" chips"));
    CHECK (contains (text, "Drag the \"Chords\", \"Bass\", \"Melody\", \"Arp\" or \"All\" chips"));
    CHECK (contains (text, "\"All\" = one track per playing part"));
    CHECK (contains (text, "\"Save\""));
    CHECK (contains (text, "Click \"Roll\""));
    CHECK (contains (text, "Melody, Arp and Bass layers"));

    // Shown by Inno Setup as InfoAfterFile: plain ASCII, lines that fit its page.
    bool ascii = true;
    for (unsigned char c : raw)
        ascii = ascii && (c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f));
    CHECK (ascii);

    size_t lineStart = 0, longest = 0;
    for (size_t i = 0; i <= raw.size(); ++i)
        if (i == raw.size() || raw[i] == '\n')
        {
            longest = std::max (longest, i - lineStart);
            lineStart = i + 1;
        }
    CHECK (longest <= 80);
}

TEST_CASE ("docs: README bass wording matches the bass mode names and Root + 5th")
{
    const auto text = folded (readRepoFile ("README.md"));
    REQUIRE (! text.empty());

    for (int m = 0; m < numBassModes; ++m)
    {
        const std::string name = bassModeName ((BassMode) m);
        if (! contains (text, name))
            cbtest::reportFailure (__FILE__, __LINE__, "README does not name the bass mode \"" + name + "\"");
    }

    CHECK (! contains (text, "Rolling kick gap"));
    CHECK (! contains (text, "plays the root or the slash-chord bass note."));
    CHECK (contains (text, "except Root + 5th, which alternates it with the chord's fifth"));
}

TEST_CASE ("docs: README says an edited layer ignores Velocity, Gate and Humanize")
{
    const auto text = folded (readRepoFile ("README.md"));
    const auto bullet = text.find ("**Timing of edited layers.**");
    REQUIRE (bullet != std::string::npos);
    const auto next = text.find ("**Old projects**", bullet);
    REQUIRE (next != std::string::npos);
    const auto section = text.substr (bullet, next - bullet);

    CHECK (contains (section, "ignores the Velocity, Gate and Humanize knobs"));
    CHECK (contains (section, "the first edit removes any jitter"));
    CHECK (contains (section, "Reset brings the knobs back"));
}

TEST_CASE ("docs: ARCHITECTURE uses the Chords | Roll label and pluginval strictness 10")
{
    const auto text = folded (readRepoFile ("ARCHITECTURE.md"));
    REQUIRE (! text.empty());

    CHECK (! contains (text, "Chords | Melody"));
    CHECK (contains (text, "the editor's \"Chords | Roll\" switch"));
    CHECK (! contains (text, "--strictness-level 8"));
    CHECK (contains (text, "--strictness-level 10"));

    // README states the same gate.
    CHECK (contains (folded (readRepoFile ("README.md")), "--strictness-level 10"));
}

TEST_CASE ("docs: the README screenshot is a current 1100x690 PNG, not the phase-1 image")
{
    const auto png = readRepoFile ("docs/screenshot.png");
    REQUIRE (png.size() > 24);
    CHECK (png.compare (0, 8, "\x89PNG\r\n\x1a\n") == 0);

    auto be32 = [&png] (size_t at)
    {
        return ((uint32_t) (unsigned char) png[at] << 24) | ((uint32_t) (unsigned char) png[at + 1] << 16)
             | ((uint32_t) (unsigned char) png[at + 2] << 8) | (uint32_t) (unsigned char) png[at + 3];
    };
    CHECK_EQ (be32 (16), 1100u);
    CHECK_EQ (be32 (20), 690u);

    // FNV-1a 64 of the phase-1 screenshot (dc299a2: "Both" chip, "Save .mid", no Chords | Roll switch).
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : png)
        h = (h ^ c) * 0x100000001b3ull;
    CHECK (h != 0x8572f7e35563e938ull);
}
