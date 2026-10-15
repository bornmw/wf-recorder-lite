#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

// Pure path/format helpers, factored out of MainWindow so they are
// unit-testable (see tests/tst_pathtools.cpp).

// Drop a known container extension (case-insensitive) so callers can force
// the one matching the selected format.
inline QString stripKnownExt(const QString &p)
{
    const QStringList known = {".mp4", ".gif", ".mkv", ".webm", ".mov"};
    for (const QString &ext : known)
        if (p.endsWith(ext, Qt::CaseInsensitive))
            return p.left(p.size() - ext.size());
    return p;
}

// Field text -> canonical path: empty falls back to "recording", a bare
// file name is placed in the home directory.
inline QString normalizeRecordPath(const QString &fieldText)
{
    QString p = fieldText.trimmed();
    if (p.isEmpty())
        p = "recording";
    if (!p.contains('/'))
        p = QDir::homePath() + "/" + p;
    return p;
}

// Human-readable byte size: "500 B" / "2 KB" / "45.2 MB".
inline QString formatSize(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    if (bytes >= 1024)
        return QString::number(bytes / 1024.0, 'f', 0) + " KB";
    return QString::number(bytes) + " B";
}

// Parse wlr-randr output for one output's PHYSICAL position and scale.
// Returns true if the output is listed; fields that are missing keep the
// defaults (x=0, y=0, scale=1.0) set on entry.
inline bool parseWlrRandr(const QString &out, const QString &name,
                          int &x, int &y, double &scale)
{
    x = 0;
    y = 0;
    scale = 1.0;
    bool found = false;
    QString cur;
    for (const QString &raw : out.split('\n'))
    {
        if (raw.isEmpty() || raw.startsWith(' '))
        {
            const QString line = raw.trimmed();
            if (cur != name)
                continue;
            found = true; // output is listed; missing fields keep the defaults
            if (line.startsWith("Position:"))
            {
                const auto parts =
                    line.mid(9).trimmed().split(',', Qt::SkipEmptyParts);
                if (parts.size() == 2)
                {
                    bool okx = false, oky = false;
                    x = parts[0].toInt(&okx);
                    y = parts[1].toInt(&oky);
                }
            }
            else if (line.startsWith("Scale:"))
                scale = line.mid(6).trimmed().toDouble();
        }
        else
        {
            cur = raw.section(' ', 0, 0);
        }
    }
    return found;
}
