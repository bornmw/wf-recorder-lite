#include <QDir>
#include <QtTest>

#include "pathtools.h"

// Unit tests for the pure helpers in pathtools.h. The GUI/process-management
// code needs a live Wayland session and is covered by the CI smoke test
// (offscreen launch) instead.
class tst_Pathtools : public QObject
{
    Q_OBJECT

private slots:
    void test_stripKnownExt()
    {
        QCOMPARE(stripKnownExt("/v/recording.mp4"), QString("/v/recording"));
        QCOMPARE(stripKnownExt("/v/recording.GIF"), QString("/v/recording"));
        QCOMPARE(stripKnownExt("/v/recording.mkv"), QString("/v/recording"));
        QCOMPARE(stripKnownExt("/v/recording.webm"), QString("/v/recording"));
        QCOMPARE(stripKnownExt("/v/recording.mov"), QString("/v/recording"));
        // Unknown or absent extensions pass through untouched.
        QCOMPARE(stripKnownExt("/v/recording.avi"), QString("/v/recording.avi"));
        QCOMPARE(stripKnownExt("/v/recording"), QString("/v/recording"));
        // The extension is not stripped from the stem.
        QCOMPARE(stripKnownExt("/v/mp4archive.mp4"), QString("/v/mp4archive"));
        QCOMPARE(stripKnownExt("/v/giftools"), QString("/v/giftools"));
    }

    void test_normalizeRecordPath()
    {
        const QString home = QDir::homePath();
        // Bare name -> home directory.
        QCOMPARE(normalizeRecordPath("recording"), home + "/recording");
        QCOMPARE(normalizeRecordPath("  clip  "), home + "/clip");
        // Empty -> "recording" in home.
        QCOMPARE(normalizeRecordPath(""), home + "/recording");
        QCOMPARE(normalizeRecordPath("   "), home + "/recording");
        // Absolute paths pass through (trimmed).
        QCOMPARE(normalizeRecordPath("/videos/clip.mp4"),
                 QString("/videos/clip.mp4"));
        QCOMPARE(normalizeRecordPath(" /videos/clip "),
                 QString("/videos/clip"));
    }

    void test_outPathExtensionForcing()
    {
        // The composed helper is what MainWindow::outPath() uses:
        // strip a known extension, then force the format's one.
        const QString home = QDir::homePath();
        QCOMPARE(stripKnownExt(normalizeRecordPath("clip.mp4")) + ".gif",
                 home + "/clip.gif");
        QCOMPARE(stripKnownExt(normalizeRecordPath("clip.GIF")) + ".mp4",
                 home + "/clip.mp4");
        QCOMPARE(stripKnownExt(normalizeRecordPath("")) + ".mp4",
                 home + "/recording.mp4");
    }

    void test_formatSize()
    {
        QCOMPARE(formatSize(0), QString("0 B"));
        QCOMPARE(formatSize(512), QString("512 B"));
        QCOMPARE(formatSize(1024), QString("1 KB"));
        QCOMPARE(formatSize(2048), QString("2 KB"));
        QCOMPARE(formatSize(1536), QString("2 KB")); // 1.5 KB rounds to 2
        QCOMPARE(formatSize(1024 * 1024), QString("1.0 MB"));
        QCOMPARE(formatSize(45 * 1024 * 1024 + 123), QString("45.0 MB"));
    }

    void test_parseWlrRandr_twoOutputs()
    {
        // Shaped like real wlr-randr output (space-indented properties).
        const QString out =
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
        QCOMPARE(parseWlrRandr(out, "eDP-1", x, y, sc), true);
        QCOMPARE(x, 0);
        QCOMPARE(y, 0);
        QCOMPARE(sc, 2.0);

        x = -1; y = -1; sc = -1;
        QCOMPARE(parseWlrRandr(out, "DP-2", x, y, sc), true);
        QCOMPARE(x, 2880);
        QCOMPARE(y, 0);
        QCOMPARE(sc, 1.0);
    }

    void test_parseWlrRandr_fractionalScale()
    {
        const QString out =
            "HDMI-A-1 connected 3840x2160 0+0+0\n"
            "  60.00* 3840x2160\n"
            "  Position: 0,0\n"
            "  Scale: 1.500000\n";
        int x, y;
        double sc;
        QCOMPARE(parseWlrRandr(out, "HDMI-A-1", x, y, sc), true);
        QCOMPARE(sc, 1.5);
    }

    void test_parseWlrRandr_missingFieldKeepsDefault()
    {
        const QString out =
            "eDP-1 connected 2880x1800 0+0+0\n"
            "  120.00* 2880x1800\n"
            "  Position: 100,200\n";
        int x = -1, y = -1;
        double sc = -1;
        QCOMPARE(parseWlrRandr(out, "eDP-1", x, y, sc), true);
        QCOMPARE(x, 100);
        QCOMPARE(y, 200);
        QCOMPARE(sc, 1.0); // default kept
    }

    void test_parseWlrRandr_unknownOutput()
    {
        const QString out =
            "eDP-1 connected 2880x1800 0+0+0\n"
            "  Position: 0,0\n"
            "  Scale: 2.000000\n";
        int x, y;
        double sc;
        QCOMPARE(parseWlrRandr(out, "DP-9", x, y, sc), false);
    }
};

QTEST_GUILESS_MAIN(tst_Pathtools)
#include "tst_pathtools.moc"
