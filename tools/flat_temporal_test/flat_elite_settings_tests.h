#pragma once

// The flat F8 panel's settings warning (src\d3d11\flat_elite_settings.h): Elite's Settings.xml and
// Custom.<major>.<minor>.fxcfg parsed from fixtures of the shapes users have (plain tags, one per
// line inside <Root>), the file chosen by the highest version, the words for the three reference
// users (AA on, bloom and DoF on, everything off), a non-Custom preset, missing files, the
// wrapping, and the watcher's re-read on change. The files are real files in a temporary
// directory, so the folder reading is the code the DLL runs; the panel's wiring is pinned by the
// source scan in flat_temporal_test.cpp.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/d3d11/flat_elite_settings.h"
#include "../../src/d3d11/flat_hdr_route.h"   // flatHdrSupersamplingAdvice: the key and sizes that decide the extra paragraph

namespace elite_settings_test {

// A temporary Options\Graphics stand-in; the destructor removes what the test wrote.
struct Folder {
    std::wstring path;
    std::vector<std::wstring> written;
    Folder() {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        wchar_t leaf[64];
        swprintf_s(leaf, L"edvr_fxcfg_test_%lu_%llu", GetCurrentProcessId(),
                   static_cast<unsigned long long>(GetTickCount64()));
        path = std::wstring(temp) + leaf;
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~Folder() {
        for (const auto& f : written) DeleteFileW(f.c_str());
        RemoveDirectoryW(path.c_str());
    }
    // Write `text` as `name`, last written `seconds` after an arbitrary fixed epoch.
    void write(const char* name, const std::string& text, uint64_t seconds) {
        const std::wstring full = path + L"\\" + std::wstring(name, name + std::strlen(name));
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        ULARGE_INTEGER t;
        t.QuadPart = 134000000000000000ull + seconds * 10000000ull;
        FILETIME ft;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
        bool known = false;
        for (const auto& f : written) known |= f == full;
        if (!known) written.push_back(full);
    }
};

inline std::string settingsXml(const char* preset) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n<Root>\r\n  <PresetName>") + preset +
           "</PresetName>\r\n  <Fullscreen>1</Fullscreen>\r\n</Root>\r\n";
}
// One tag per line inside <Root>, as Elite writes them.
inline std::string fxcfg(int aa, int bloom, int dof) {
    char text[512];
    std::snprintf(text, sizeof(text),
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n<Root>\r\n  <BlurEnabled>false</BlurEnabled>\r\n"
        "  <AAMode>%d</AAMode>\r\n  <AOQuality>3</AOQuality>\r\n  <BloomQuality>%d</BloomQuality>\r\n"
        "  <DOFEnabled>%d</DOFEnabled>\r\n  <HMDRenderTargetMultiplier>1.0</HMDRenderTargetMultiplier>\r\n</Root>\r\n",
        aa, bloom, dof);
    return text;
}
// The panel's ruler, a fixed 20 px a character.
inline int ruler(const char* text, void*) { return static_cast<int>(std::strlen(text)) * 20; }
constexpr int kPanelWidthPx = 806;   // the flat card less its padding, at the default size

}  // namespace elite_settings_test

inline int flatEliteSettingsTests() {
    using namespace edvr;
    using elite_settings_test::Folder;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: elite settings %s\n", name); ++failures; }
    };
    auto words = [&](const char* mode, const EliteGraphics& g, int width, FlatSettingsWarning* out) {
        flatComposeSettingsWarning(mode, g, width, &elite_settings_test::ruler, nullptr, out);
    };

    // ---- the tags ----------------------------------------------------------------
    {
        const std::string text = elite_settings_test::fxcfg(4, 3, 2);
        int v = -9;
        expect(flatXmlInt(text, "AAMode", &v) && v == 4 && flatXmlInt(text, "BloomQuality", &v) && v == 3 &&
               flatXmlInt(text, "DOFEnabled", &v) && v == 2, "plain one-per-line tags parse to their numbers");
        expect(!flatXmlInt(text, "NotThere", &v) && !flatXmlInt("<AAMode></AAMode>", "AAMode", &v) &&
               !flatXmlInt("<AAMode>x</AAMode>", "AAMode", &v), "a missing, empty or non-numeric tag is not a value");
        expect(flatXmlInt("<A> true </A>", "A", &v) && v == 1 && flatXmlInt("<A>false</A>", "A", &v) && v == 0 &&
               flatXmlInt("<A>\r\n  7\r\n</A>", "A", &v) && v == 7, "true/false dialect and whitespace");
        std::string preset;
        expect(flatXmlText(elite_settings_test::settingsXml("Custom"), "PresetName", &preset) && preset == "Custom",
               "Settings.xml's PresetName");
        // <AAModeX> must not answer for <AAMode>.
        expect(!flatXmlInt("<AAModeX>4</AAModeX>", "AAMode", &v), "a longer tag name is another tag");
    }

    // ---- the file names --------------------------------------------------------------
    {
        unsigned a = 9, b = 9;
        expect(flatCustomFxcfgVersion("Custom.4.4.fxcfg", &a, &b) && a == 4 && b == 4 &&
               flatCustomFxcfgVersion("Custom.10.2.fxcfg", &a, &b) && a == 10 && b == 2 &&
               flatCustomFxcfgVersion("custom.4.0.FXCFG", &a, &b) && a == 4 && b == 0 &&
               flatCustomFxcfgVersion("Custom.fxcfg", &a, &b) && a == 0 && b == 0 &&
               flatCustomFxcfgVersion("Custom.4.fxcfg", &a, &b) && a == 4 && b == 0,
               "Custom.<major>.<minor>.fxcfg parses, case-insensitively, with the unversioned forms");
        expect(!flatCustomFxcfgVersion("High.4.4.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom.4.4.fxcfg.bak", &a, &b) &&
               !flatCustomFxcfgVersion("Custom.a.b.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom..fxcfg", &a, &b) &&
               !flatCustomFxcfgVersion("Custom.4.4.4.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom.4..fxcfg", &a, &b) &&
               !flatCustomFxcfgVersion("Settings.xml", &a, &b),
               "another preset, a backup suffix, letters or a third number are not versions");
        using F = EliteFxcfg;
        // The highest version wins whatever the write times say (user 3 had 4.0 to 4.4 side by side).
        std::vector<F> files = {{"Custom.4.0.fxcfg", 900}, {"Custom.4.4.fxcfg", 100}, {"Custom.4.2.fxcfg", 500},
                                {"High.4.4.fxcfg", 9999}, {"Custom.4.1.fxcfg", 800}};
        expect(flatPickCustomFxcfg(files) == 1, "the highest <major>.<minor> is the one the game reads, not the newest file");
        files = {{"Custom.9.9.fxcfg", 1}, {"Custom.10.0.fxcfg", 2}};
        expect(flatPickCustomFxcfg(files) == 1, "versions compare as numbers: 10.0 is above 9.9");
        files = {{"Custom.4.4.fxcfg", 10}, {"Custom.4.4.fxcfg", 30}};
        expect(flatPickCustomFxcfg(files) == 1, "the same version twice: the newest write breaks the tie");
        files = {{"Custom.beta.fxcfg", 10}, {"CustomOld.fxcfg", 30}, {"High.fxcfg", 99}};
        expect(flatPickCustomFxcfg(files) == 1, "no name carries a version: the newest Custom file by write time");
        files = {{"Custom.beta.fxcfg", 10}, {"Custom.3.1.fxcfg", 5}};
        expect(flatPickCustomFxcfg(files) == 1, "a versioned name beats an unparsed newer one");
        files = {{"High.4.4.fxcfg", 5}, {"Ultra.4.4.fxcfg", 6}};
        expect(flatPickCustomFxcfg(files) == -1 && flatPickCustomFxcfg({}) == -1, "no Custom file: none");
    }

    // ---- the words for the three reference users ------------------------------------------
    {
        EliteGraphics user1, user2, sean;
        for (EliteGraphics* g : {&user1, &user2, &sean}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        user1.aaMode = 4; user1.bloomQuality = 0; user1.dofEnabled = 0;
        user2.aaMode = 0; user2.bloomQuality = 3; user2.dofEnabled = 2;
        sean.aaMode = 0; sean.bloomQuality = 0; sean.dofEnabled = 0;
        const int wide = 100000;   // no wrapping: one line per paragraph, for the exact words
        FlatSettingsWarning w;
        words("DLSS", user1, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "DLSS is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "user 1 (AAMode 4, bloom and DoF off): names only Anti-aliasing");
        words("DLAA", user2, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "DLAA is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Bloom, Depth of field") == 0,
               "user 2 (AA off, BloomQuality 3, DOFEnabled 2): names Bloom and Depth of field");
        words("FSR3", sean, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "FSR3 is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Please send your logs (F10 in the cockpit, then the installer's log bundle).") == 0,
               "Sean (all off): asks for the logs and names no setting");
        EliteGraphics all = user1;
        all.bloomQuality = 1; all.dofEnabled = 1;
        words("TAA", all, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field") == 0,
               "all three on: all three named, in that order");

        EliteGraphics preset = sean;
        preset.custom = false;
        std::strcpy(preset.preset, "Ultra");
        words("DLSS", preset, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Ultra graphics preset may turn on") != nullptr &&
               std::strstr(w.line[1], "Anti-aliasing, Bloom or Depth of field") != nullptr &&
               std::strstr(w.line[1], "Please send your logs") == nullptr,
               "a preset other than Custom is named and said to be able to turn the effects on");

        EliteGraphics unknown;   // nothing read at all
        words("DLSS", unknown, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "settings that could not be read ask for the logs");
        EliteGraphics customNoFile = sean;
        customNoFile.fileRead = false; customNoFile.aaMode = customNoFile.bloomQuality = customNoFile.dofEnabled = -1;
        words("DLSS", customNoFile, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "Custom with no readable .fxcfg asks for the logs");

        // The panel's width: 806 px at 20 px a character is 40 characters a line.
        words("DLSS", user2, elite_settings_test::kPanelWidthPx, &w);
        bool fits = w.count >= 3 && w.count <= FlatSettingsWarning::kMaxLines;
        std::string joined0, joined1;
        for (int i = 0; i < w.count; ++i) {
            if (elite_settings_test::ruler(w.line[i], nullptr) > elite_settings_test::kPanelWidthPx) fits = false;
        }
        expect(fits, "wrapped to the panel's width, every line fits");
        std::string all2;
        for (int i = 0; i < w.count; ++i) all2 += (i ? " " : "") + std::string(w.line[i]);
        expect(all2.find("DLAA") == std::string::npos && all2.find("DLSS is not active: Elite's post-processing is not "
               "recognised. Turn off in Elite's graphics options: Bloom, Depth of field") != std::string::npos,
               "the wrapped lines read as the two sentences, words unchanged");
        // A change of mode, preset or any of the three fields changes the key (and so the raster and the log).
        expect(flatSettingsWarningKey("DLSS", user1) != flatSettingsWarningKey("DLAA", user1) &&
               flatSettingsWarningKey("DLSS", user1) != flatSettingsWarningKey("DLSS", user2) &&
               flatSettingsWarningKey("DLSS", sean) != flatSettingsWarningKey("DLSS", preset) &&
               flatSettingsWarningKey("DLSS", user1) == flatSettingsWarningKey("DLSS", user1),
               "the warning key moves with the mode, the preset and the three fields");
    }

    // ---- the HDR route is active (section 81): Bloom and Depth of field are not what a refusal is about -----------
    // The route resolves before both, so their advice goes; the Anti-aliasing advice stays (the game's own AA after
    // the tone double-filters, and a game TAA's jitter fights EDVR's). Every other word is unchanged, which the
    // route-off rows above pin.
    {
        EliteGraphics user1, user2, all, preset;
        for (EliteGraphics* g : {&user1, &user2, &all, &preset}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        user1.aaMode = 4; user1.bloomQuality = 0; user1.dofEnabled = 0;
        user2.aaMode = 0; user2.bloomQuality = 3; user2.dofEnabled = 2;
        all.aaMode = 4; all.bloomQuality = 1; all.dofEnabled = 1;
        preset.aaMode = preset.bloomQuality = preset.dofEnabled = 0;
        preset.custom = false; std::strcpy(preset.preset, "Ultra");
        const int wide = 100000;
        FlatSettingsWarning w;
        auto hdrWords = [&](const char* mode, const EliteGraphics& g) {
            flatComposeSettingsWarning(mode, g, wide, &elite_settings_test::ruler, nullptr, &w, true);
        };
        hdrWords("DLSS", user1);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "HDR route active, user 1 (AA on): Anti-aliasing is still named");
        hdrWords("DLAA", user2);
        expect(w.count == 2 && std::strstr(w.line[1], "Bloom") == nullptr && std::strstr(w.line[1], "Depth of field") == nullptr &&
               std::strstr(w.line[1], "Please send your logs") != nullptr,
               "HDR route active, user 2 (only bloom and DoF on): neither is named, the logs are asked for");
        hdrWords("TAA", all);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "HDR route active, all three on: only Anti-aliasing is named");
        hdrWords("DLSS", preset);
        expect(w.count == 2 && std::strstr(w.line[1], "Ultra graphics preset may turn on Anti-aliasing.") != nullptr &&
               std::strstr(w.line[1], "Bloom") == nullptr && std::strstr(w.line[1], "Depth of field") == nullptr,
               "HDR route active, another preset: only Anti-aliasing is said to be turned on");
        words("TAA", all, wide, &w);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field") == 0,
               "HDR route not active: the words are exactly what they were (all three named)");
        expect(flatSettingsWarningKey("DLSS", user1, false) != flatSettingsWarningKey("DLSS", user1, true) &&
               flatSettingsWarningKey("DLSS", user1) == flatSettingsWarningKey("DLSS", user1, false),
               "the warning key moves with the route, so the panel rebuilds when it flips");
    }

    // ---- supersampling below 1.0 (section 81): the warning's third paragraph ------------------------------------------
    // Frames are refused, the route's key is auto, and the route leaves them to the copy route only because the game
    // renders below the output (Elite's supersampling under 1.0): the warning says so after the advice it already gives,
    // which stays. The measured render and output sizes decide it, never Elite's settings file, and every other
    // combination leaves the words exactly as they were.
    {
        EliteGraphics user1, user2, sean, all, preset;
        for (EliteGraphics* g : {&user1, &user2, &sean, &all, &preset}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        user1.aaMode = 4; user1.bloomQuality = 0; user1.dofEnabled = 0;
        user2.aaMode = 0; user2.bloomQuality = 3; user2.dofEnabled = 2;
        sean.aaMode = 0; sean.bloomQuality = 0; sean.dofEnabled = 0;
        all.aaMode = 4; all.bloomQuality = 1; all.dofEnabled = 1;
        preset.aaMode = preset.bloomQuality = preset.dofEnabled = 0;
        preset.custom = false; std::strcpy(preset.preset, "Ultra");
        const int wide = 100000;
        FlatSettingsWarning plain, with;
        const std::string ssWords = kFlatSupersamplingWords;
        expect(ssWords == "Supersampling is below 1.0. At 1.0 or above, EDVR anti-aliases before bloom and depth of field, so they "
                        "no longer block it. Raising it costs GPU time.",
               "the extra paragraph's words: supersampling, what 1.0 or above gives, what it costs");

        // The words: one paragraph after the advice, for every advice the warning gives.
        bool after = true, unchanged = true;
        const EliteGraphics* users[] = {&user1, &user2, &sean, &all, &preset};
        for (const EliteGraphics* g : users) {
            flatComposeSettingsWarning("DLSS", *g, wide, &elite_settings_test::ruler, nullptr, &plain, false, false);
            flatComposeSettingsWarning("DLSS", *g, wide, &elite_settings_test::ruler, nullptr, &with, false, true);
            after = after && plain.count == 2 && with.count == 3 && ssWords == with.line[2];
            unchanged = unchanged && std::strcmp(plain.line[0], with.line[0]) == 0 && std::strcmp(plain.line[1], with.line[1]) == 0;
        }
        expect(after, "the supersampling paragraph is the third line after the two the warning always has");
        expect(unchanged, "and the bloom, depth-of-field and game-AA advice before it is not touched by it");
        flatComposeSettingsWarning("DLAA", user2, wide, &elite_settings_test::ruler, nullptr, &with, false, true);
        expect(with.count == 3 && std::strcmp(with.line[1], "Turn off in Elite's graphics options: Bloom, Depth of field") == 0,
               "user 2 (bloom and DoF on): both still named, then the supersampling words");
        // The route being active is nothing for it to say: the route is treating the frames, so supersampling is not in the way.
        flatComposeSettingsWarning("DLAA", user2, wide, &elite_settings_test::ruler, nullptr, &with, true, true);
        expect(with.count == 2 && std::strstr(with.line[1], "Supersampling") == nullptr,
               "with the route active the paragraph never appears, whatever the flag says");
        // At the panel's width it wraps onto the card: every variant fits the lines (ten at worst, twelve allowed), each line fits the width, and
        // the flat page's three rows and a blank line still leave the card room (menu.cpp static_asserts the same sum).
        bool fits = true;
        for (const EliteGraphics* g : users) {
            flatComposeSettingsWarning("TAA", *g, elite_settings_test::kPanelWidthPx, &elite_settings_test::ruler, nullptr, &with, false, true);
            fits = fits && with.count >= 4 && with.count <= 10 && with.count <= FlatSettingsWarning::kMaxLines;
            for (int i = 0; i < with.count; ++i)
                if (elite_settings_test::ruler(with.line[i], nullptr) > elite_settings_test::kPanelWidthPx) fits = false;
            std::string joined;
            for (int i = 0; i < with.count; ++i) joined += (i ? " " : "") + std::string(with.line[i]);
            fits = fits && joined.find(ssWords) != std::string::npos;   // the wrapped lines read as the words, none dropped
        }
        expect(fits, "wrapped to the panel's width every variant fits the lines it has, and the paragraph is whole");
        expect(3 + 1 + FlatSettingsWarning::kMaxLines <= 16, "the flat page's rows, a blank line and a full warning fit the card's 16 lines");

        // The key moves with the paragraph, so the panel and the log update live when supersampling crosses 1.0; with the
        // route active the flag changes nothing (there is no paragraph to add).
        expect(flatSettingsWarningKey("DLSS", user2, false, true) != flatSettingsWarningKey("DLSS", user2, false, false) &&
               flatSettingsWarningKey("DLSS", user2, true, true) == flatSettingsWarningKey("DLSS", user2, true, false) &&
               flatSettingsWarningKey("DLSS", user2, false, false) == flatSettingsWarningKey("DLSS", user2) &&
               flatSettingsWarningKey("DLSS", user2, false, true) != flatSettingsWarningKey("DLSS", user2, true, false),
               "the warning key moves with the supersampling paragraph and with nothing else it does not show");

        // The whole truth table, from what the runtime measures and publishes to the words: key x sizes x refusing x route.
        // The paragraph appears for exactly one row family: refused, key auto, route not treating, R below D.
        struct Size { uint32_t rw, rh; const char* name; };
        const Size sizes[] = {{3072, 1728, "R < D"}, {3840, 2160, "R = D"}, {4800, 2700, "R > D"}};
        bool table = true;
        int shown = 0;
        for (const FlatHdrKey key : {FlatHdrKey::Off, FlatHdrKey::Auto})
            for (const Size& size : sizes)
                for (int refusing = 0; refusing < 2; ++refusing)
                    for (int route = 0; route < 2; ++route) {
                        const bool published = flatHdrSupersamplingAdvice(key, route != 0, size.rw, size.rh, 3840, 2160);
                        const FlatWarningFlags f = flatWarningFlags(refusing != 0, route != 0, published);
                        bool present = false;
                        if (f.refusing) {   // the panel composes nothing when frames are not refused
                            flatComposeSettingsWarning("DLSS", user2, wide, &elite_settings_test::ruler, nullptr, &with,
                                                       f.hdrRoute, f.supersamplingBelowOne);
                            present = with.count == 3 && ssWords == with.line[2];
                        }
                        const bool want = refusing != 0 && key == FlatHdrKey::Auto && route == 0 && size.rw < 3840;
                        if (present != want || f.supersamplingBelowOne != (want && f.refusing)) table = false;
                        if (present) ++shown;
                    }
        expect(table && shown == 1,
               "present for refused + R < D + key auto and nowhere else: not with the key off, at R = D or above, with the "
               "route treating, or when frames are not refused");

        // The log line carries every paragraph the panel does, names the measured sizes with the extra one, and is
        // otherwise exactly what it was.
        char line[900];
        FlatSettingsWarning logged;
        const FlatWarningFlags on = flatWarningFlags(true, false, true);
        flatComposeSettingsWarning("DLSS", user2, 0, nullptr, nullptr, &logged, on.hdrRoute, on.supersamplingBelowOne);
        flatFormatSettingsWarningLog(line, sizeof(line), false, "DLSS", "no-known-tone-pass", true, on, 3072, 1728, 3840, 2160, logged);
        const std::string onText = line;
        expect(onText == "flat settings warning: shown (mode=DLSS, frames refused for no-known-tone-pass, work stood down, "
                         "supersampling below 1.0 (render 3072x1728, output 3840x2160)): DLSS is not active: Elite's "
                         "post-processing is not recognised. Turn off in Elite's graphics options: Bloom, Depth of field " +
                             ssWords,
               "the log line for a refused frame below the output: the conditions, the sizes, all three paragraphs");
        const FlatWarningFlags off = flatWarningFlags(true, false, false);
        flatComposeSettingsWarning("DLSS", user2, 0, nullptr, nullptr, &logged, off.hdrRoute, off.supersamplingBelowOne);
        flatFormatSettingsWarningLog(line, sizeof(line), true, "DLSS", "no-known-tone-pass", true, off, 0, 0, 0, 0, logged);
        expect(std::string(line) == "flat settings warning: changed (mode=DLSS, frames refused for no-known-tone-pass, work stood "
                                    "down): DLSS is not active: Elite's post-processing is not recognised. Turn off in Elite's "
                                    "graphics options: Bloom, Depth of field",
               "without the paragraph the log line is what it always was");
        const FlatWarningFlags routeOn = flatWarningFlags(true, true, true);
        flatComposeSettingsWarning("DLSS", user1, 0, nullptr, nullptr, &logged, routeOn.hdrRoute, routeOn.supersamplingBelowOne);
        flatFormatSettingsWarningLog(line, sizeof(line), false, "DLSS", "no-hdr-consumer", false, routeOn, 3072, 1728, 3840, 2160, logged);
        expect(std::string(line).find("HDR route active") != std::string::npos &&
                   std::string(line).find("supersampling") == std::string::npos &&
                   std::string(line).find("Supersampling") == std::string::npos,
               "with the route active the log line says so and names no supersampling");
        expect(!flatWarningFlags(false, false, true).supersamplingBelowOne && !flatWarningFlags(false, true, true).hdrRoute,
               "frames not refused: no condition holds, whatever the runtime published");
    }

    // ---- real files ------------------------------------------------------------------------
    {
        Folder dir;
        dir.write("Settings.xml", elite_settings_test::settingsXml("Custom"), 10);
        // Custom 4.0 through 4.4 side by side, the OLD one written last (user 3): the game reads 4.4.
        dir.write("Custom.4.4.fxcfg", elite_settings_test::fxcfg(0, 0, 0), 20);
        dir.write("Custom.4.3.fxcfg", elite_settings_test::fxcfg(2, 2, 2), 30);
        dir.write("Custom.4.0.fxcfg", elite_settings_test::fxcfg(4, 4, 4), 90);
        dir.write("Custom.4.4.fxcfg.baseline-bak-20260921", elite_settings_test::fxcfg(4, 4, 4), 95);
        dir.write("High.4.4.fxcfg", elite_settings_test::fxcfg(4, 4, 4), 99);
        const EliteFolderListing listing = flatListEliteGraphics(dir.path);
        expect(listing.folder && listing.settings && listing.fxcfg.size() == 4,
               "the listing sees Settings.xml and the four .fxcfg files, not the backup");
        const EliteGraphics g = flatReadEliteGraphics(dir.path, listing);
        expect(g.folderFound && g.presetKnown && g.custom && g.fileRead &&
               std::strcmp(g.preset, "Custom") == 0 && std::strcmp(g.file, "Custom.4.4.fxcfg") == 0 &&
               g.aaMode == 0 && g.bloomQuality == 0 && g.dofEnabled == 0 && !g.anyOn(),
               "the highest Custom version is read: Custom.4.4.fxcfg, all three off");
        char line[512];
        flatFormatEliteSettings(line, sizeof(line), g);
        expect(std::string(line) == "flat settings: Elite graphics preset=Custom file=Custom.4.4.fxcfg AAMode=0 "
               "BloomQuality=0 DOFEnabled=0", "the log line for what was read");

        // The watcher: reads once, then only when a file changes, checked at most every 2 s.
        FlatSettingsWatcher watcher;
        watcher.setFolder(dir.path);
        expect(!watcher.everRead() && watcher.poll(1000, false) && watcher.everRead() && watcher.reads() == 1,
               "the first poll reads");
        expect(!watcher.poll(1500, false) && watcher.reads() == 1, "a poll inside 2 s does not look at all");
        expect(!watcher.poll(3500, false) && watcher.reads() == 1, "a poll after 2 s finds nothing changed and reads nothing");
        expect(!watcher.poll(3600, true) && watcher.reads() == 1, "a forced poll (the menu opening) still reads only on change");
        // The game rewrites the current file when settings are applied.
        dir.write("Custom.4.4.fxcfg", elite_settings_test::fxcfg(4, 3, 2), 200);
        expect(!watcher.poll(3700, false), "inside the 2 s window even a change waits");
        expect(watcher.poll(5700, false) && watcher.reads() == 2 &&
               watcher.settings().aaMode == 4 && watcher.settings().bloomQuality == 3 &&
               watcher.settings().dofEnabled == 2 && watcher.settings().anyOn(),
               "a rewritten file is read again at the next check");
        // A newer game version writes a higher file: it becomes the one read.
        dir.write("Custom.4.5.fxcfg", elite_settings_test::fxcfg(0, 0, 0), 210);
        expect(watcher.poll(6000, true) && watcher.reads() == 3 &&
               std::strcmp(watcher.settings().file, "Custom.4.5.fxcfg") == 0 && !watcher.settings().anyOn(),
               "a new higher version is picked up, forced");
        // The preset changes to one that is not Custom.
        dir.write("Settings.xml", elite_settings_test::settingsXml("High"), 220);
        expect(watcher.poll(9000, false) && watcher.reads() == 4 && watcher.settings().presetKnown &&
               !watcher.settings().custom && !watcher.settings().fileRead &&
               std::strcmp(watcher.settings().preset, "High") == 0,
               "a preset other than Custom is read as such and no .fxcfg is read for it");
        flatFormatEliteSettings(line, sizeof(line), watcher.settings());
        expect(std::string(line).find("preset=High (not Custom") != std::string::npos, "and the log line says so");
    }
    {   // No folder, no Settings.xml, an empty folder.
        Folder dir;
        FlatSettingsWatcher watcher;
        watcher.setFolder(dir.path);
        expect(watcher.poll(0, false) && watcher.settings().folderFound && !watcher.settings().presetKnown &&
               !watcher.settings().fileRead, "an empty Options\\Graphics folder: found, nothing in it");
        char line[512];
        flatFormatEliteSettings(line, sizeof(line), watcher.settings());
        expect(std::string(line).find("preset unknown") != std::string::npos, "and the log line says the preset is unknown");
        FlatSettingsWatcher missing;
        missing.setFolder(dir.path + L"\\not_here");
        expect(missing.poll(0, false) && !missing.settings().folderFound, "a folder that does not exist");
        flatFormatEliteSettings(line, sizeof(line), missing.settings());
        expect(std::string(line).find("no Elite graphics settings folder") != std::string::npos,
               "and the log line says there is none");
        FlatSettingsWarning w;
        flatComposeSettingsWarning("DLSS", missing.settings(), 100000, &elite_settings_test::ruler, nullptr, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "with no settings folder the warning asks for the logs");
        FlatSettingsWatcher blank;   // LOCALAPPDATA unset: an empty folder path
        blank.setFolder(std::wstring());
        expect(blank.poll(0, false) && !blank.settings().folderFound, "an empty folder path is no folder");
    }
    // The folder composition the installer's log bundler shares.
    expect(eliteGraphicsFolderUnder(L"C:\\Users\\a\\AppData\\Local") ==
               L"C:\\Users\\a\\AppData\\Local\\Frontier Developments\\Elite Dangerous\\Options\\Graphics" &&
           eliteGraphicsFolderUnder(L"C:\\Users\\a\\AppData\\Local\\") ==
               L"C:\\Users\\a\\AppData\\Local\\Frontier Developments\\Elite Dangerous\\Options\\Graphics" &&
           eliteGraphicsFolderUnder(L"").empty(),
           "the shared folder composition: with or without a trailing separator, and empty in, empty out");
    return failures;
}
