// Unit tests for the pure helpers in pathtools.h (plain C++ — no framework).
// The GUI/process-management code needs a live Wayland session and is
// covered by the CI smoke test (xvfb launch) instead.
#include "pathtools.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(expr)                                                        \
    do                                                                     \
    {                                                                      \
        if (!(expr))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do                                                                     \
    {                                                                      \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb))                                                   \
        {                                                                  \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a,  \
                        #b);                                               \
        }                                                                  \
    } while (0)

static void
test_stripKnownExt()
{
    CHECK_EQ(stripKnownExt("/v/recording.mp4"), std::string("/v/recording"));
    CHECK_EQ(stripKnownExt("/v/recording.GIF"), std::string("/v/recording"));
    CHECK_EQ(stripKnownExt("/v/recording.mkv"), std::string("/v/recording"));
    CHECK_EQ(stripKnownExt("/v/recording.webm"), std::string("/v/recording"));
    CHECK_EQ(stripKnownExt("/v/recording.mov"), std::string("/v/recording"));
    // Unknown or absent extensions pass through untouched.
    CHECK_EQ(stripKnownExt("/v/recording.avi"), std::string("/v/recording.avi"));
    CHECK_EQ(stripKnownExt("/v/recording"), std::string("/v/recording"));
    // The extension is not stripped from the stem.
    CHECK_EQ(stripKnownExt("/v/mp4archive.mp4"), std::string("/v/mp4archive"));
    CHECK_EQ(stripKnownExt("/v/giftools"), std::string("/v/giftools"));
}

static void
test_normalizeRecordPath()
{
    const std::string home = homePath();
    // Bare name -> home directory.
    CHECK_EQ(normalizeRecordPath("recording"), home + "/recording");
    CHECK_EQ(normalizeRecordPath("  clip  "), home + "/clip");
    // Empty -> "recording" in home.
    CHECK_EQ(normalizeRecordPath(""), home + "/recording");
    CHECK_EQ(normalizeRecordPath("   "), home + "/recording");
    // Absolute paths pass through (trimmed).
    CHECK_EQ(normalizeRecordPath("/videos/clip.mp4"), std::string("/videos/clip.mp4"));
    CHECK_EQ(normalizeRecordPath(" /videos/clip "), std::string("/videos/clip"));
}

static void
test_outPathExtensionForcing()
{
    // The composed helper is what MainWindow::outPath() uses:
    // strip a known extension, then force the format's one.
    const std::string home = homePath();
    CHECK_EQ(stripKnownExt(normalizeRecordPath("clip.mp4")) + ".gif",
             home + "/clip.gif");
    CHECK_EQ(stripKnownExt(normalizeRecordPath("clip.GIF")) + ".mp4",
             home + "/clip.mp4");
    CHECK_EQ(stripKnownExt(normalizeRecordPath("")) + ".mp4",
             home + "/recording.mp4");
}

static void
test_formatSize()
{
    CHECK_EQ(formatSize(0), std::string("0 B"));
    CHECK_EQ(formatSize(512), std::string("512 B"));
    CHECK_EQ(formatSize(1024), std::string("1 KB"));
    CHECK_EQ(formatSize(2048), std::string("2 KB"));
    CHECK_EQ(formatSize(1536), std::string("2 KB")); // 1.5 KB rounds to 2
    CHECK_EQ(formatSize(1024 * 1024), std::string("1.0 MB"));
    CHECK_EQ(formatSize(45 * 1024 * 1024 + 123), std::string("45.0 MB"));
}

static void
test_parseWlrRandr_twoOutputs()
{
    // Shaped like real wlr-randr output (space-indented properties).
    const std::string out =
        "eDP-1 connected 2880x1800 0+0+0 (primary)\n"
        "  120.00* 2880x1800\n"
        "  Position: 0,0\n"
        "  Scale: 2.000000\n"
        "DP-2 connected 2560x1440 2880+0+0\n"
        "  60.00* 2560x1440\n"
        "  Position: 2880,0\n"
        "  Scale: 1.000000\n";

    int x = -1, y = -1;
    double sc = -1;
    CHECK(parseWlrRandr(out, "eDP-1", x, y, sc));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);
    CHECK_EQ(sc, 2.0);

    x = -1; y = -1; sc = -1;
    CHECK(parseWlrRandr(out, "DP-2", x, y, sc));
    CHECK_EQ(x, 2880);
    CHECK_EQ(y, 0);
    CHECK_EQ(sc, 1.0);
}

static void
test_parseWlrRandr_fractionalScale()
{
    const std::string out =
        "HDMI-A-1 connected 3840x2160 0+0+0\n"
        "  60.00* 3840x2160\n"
        "  Position: 0,0\n"
        "  Scale: 1.500000\n";
    int x, y;
    double sc;
    CHECK(parseWlrRandr(out, "HDMI-A-1", x, y, sc));
    CHECK_EQ(sc, 1.5);
}

static void
test_parseWlrRandr_missingFieldKeepsDefault()
{
    const std::string out =
        "eDP-1 connected 2880x1800 0+0+0\n"
        "  120.00* 2880x1800\n"
        "  Position: 100,200\n";
    int x = -1, y = -1;
    double sc = -1;
    CHECK(parseWlrRandr(out, "eDP-1", x, y, sc));
    CHECK_EQ(x, 100);
    CHECK_EQ(y, 200);
    CHECK_EQ(sc, 1.0); // default kept
}

static void
test_parseWlrRandr_unknownOutput()
{
    const std::string out =
        "eDP-1 connected 2880x1800 0+0+0\n"
        "  Position: 0,0\n"
        "  Scale: 2.000000\n";
    int x, y;
    double sc;
    CHECK(!parseWlrRandr(out, "DP-9", x, y, sc));
}

// Shaped like real wlr-randr output on this box (modes block, " px" suffix).
static const char *REAL_WLR_RANDR =
    "eDP-1 \"Samsung Display Corp. 0x4197 (eDP-1)\"\n"
    "  Make: Samsung Display Corp.\n"
    "  Model: 0x4197\n"
    "  Physical size: 300x190 mm\n"
    "  Enabled: yes\n"
    "  Modes:\n"
    "    2880x1800 px, 120.000999 Hz (preferred, current)\n"
    "    2880x1800 px, 120.000000 Hz (preferred)\n"
    "    2880x1800 px, 48.000999 Hz\n"
    "  Position: 0,0\n"
    "  Transform: normal\n"
    "  Scale: 2.000000\n"
    "  Adaptive Sync: disabled\n";

static void
test_parseWlrRandrFirstOutput()
{
    CHECK_EQ(parseWlrRandrFirstOutput(REAL_WLR_RANDR), std::string("eDP-1"));
    CHECK_EQ(
        parseWlrRandrFirstOutput(
            "DP-2 \"Other\"\n  Scale: 1.0\n"
            "HDMI-A-1 \"Other 2\"\n  Scale: 1.5\n"),
        std::string("DP-2"));
    CHECK_EQ(parseWlrRandrFirstOutput("  no outputs\n"), std::string(""));
    CHECK_EQ(parseWlrRandrFirstOutput(""), std::string(""));
}

static void
test_parseWlrRandrMode()
{
    int w = -1, h = -1;
    // The "current" mode wins even when listed first here.
    CHECK(parseWlrRandrMode(REAL_WLR_RANDR, "eDP-1", w, h));
    CHECK_EQ(w, 2880);
    CHECK_EQ(h, 1800);
    // Unknown output.
    CHECK(!parseWlrRandrMode(REAL_WLR_RANDR, "DP-9", w, h));
    // A "Physical size: 300x190 mm" line must not be mistaken for a mode
    // (no " px" suffix); fall back to the first mode of the block.
    const std::string noCurrent =
        "eDP-1 \"S\"\n"
        "  Physical size: 300x190 mm\n"
        "  Modes:\n"
        "    1920x1080 px, 60.000000 Hz (preferred)\n"
        "    1280x720 px, 60.000000 Hz\n"
        "  Scale: 1.000000\n";
    CHECK(parseWlrRandrMode(noCurrent, "eDP-1", w, h));
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1080);
}

int
main()
{
    test_stripKnownExt();
    test_normalizeRecordPath();
    test_outPathExtensionForcing();
    test_formatSize();
    test_parseWlrRandr_twoOutputs();
    test_parseWlrRandr_fractionalScale();
    test_parseWlrRandr_missingFieldKeepsDefault();
    test_parseWlrRandr_unknownOutput();
    test_parseWlrRandrFirstOutput();
    test_parseWlrRandrMode();

    if (g_failures == 0)
    {
        std::printf("All pathtools tests passed.\n");
        return 0;
    }
    std::printf("%d check(s) FAILED.\n", g_failures);
    return 1;
}
