/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singeriesettings.h"

#include "configfile.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QSettings>

#include <algorithm>

Q_LOGGING_CATEGORY(lcSingerieSettings, "nextcloud.sync.singeriesettings", QtInfoMsg)

namespace OCC
{

namespace
{

struct Values {
    qint64 uploadLimitKBps = -1;
    qint64 downloadLimitKBps = -1;
    qint64 keepFreeBytes = -1;
    bool syncPaused = false;
    qint64 transfersParallel = -1;
    bool bulkUpload = false;
    qint64 prefetchParallel = -1;
    qint64 binRetentionDays = -1;
    qint64 binMaxBytes = -1;
};

QMutex mutex;
QString overridePath;
QString loadedPath;
QDateTime loadedModified;
qint64 loadedSize = -1;
Values values;

qint64 readNumber(const QSettings &settings, const QString &key)
{
    if (!settings.contains(key)) {
        return -1;
    }
    bool ok = false;
    const auto value = settings.value(key).toLongLong(&ok);
    if (!ok || value < 0) {
        qCWarning(lcSingerieSettings) << "Ignoring" << key << "in" << settings.fileName() << ": not a number of 0 or more";
        return -1;
    }
    return value;
}

Values current()
{
    QMutexLocker locker(&mutex);
    const auto path = SingerieSettings::filePath();
    const QFileInfo info(path);
    const auto modified = info.exists() ? info.lastModified() : QDateTime{};
    const auto size = info.exists() ? info.size() : -1;
    if (path == loadedPath && modified == loadedModified && size == loadedSize) {
        return values;
    }
    loadedPath = path;
    loadedModified = modified;
    loadedSize = size;
    values = {};
    if (info.exists()) {
        const QSettings settings(path, QSettings::IniFormat);
        values.uploadLimitKBps = readNumber(settings, QStringLiteral("transfers/uploadLimitKBps"));
        values.downloadLimitKBps = readNumber(settings, QStringLiteral("transfers/downloadLimitKBps"));
        values.keepFreeBytes = readNumber(settings, QStringLiteral("space/keepFreeBytes"));
        values.syncPaused = settings.value(QStringLiteral("sync/paused"), false).toBool();
        values.transfersParallel = readNumber(settings, QStringLiteral("transfers/parallel"));
        values.bulkUpload = settings.value(QStringLiteral("transfers/bulkUpload"), false).toBool();
        values.prefetchParallel = readNumber(settings, QStringLiteral("prefetch/parallel"));
        values.binRetentionDays = readNumber(settings, QStringLiteral("bin/retentionDays"));
        values.binMaxBytes = readNumber(settings, QStringLiteral("bin/maxBytes"));
        qCInfo(lcSingerieSettings) << "Read" << path << "upload limit" << values.uploadLimitKBps << "download limit" << values.downloadLimitKBps << "keep free"
                                   << values.keepFreeBytes;
    }
    return values;
}

} // namespace

QString SingerieSettings::filePath()
{
    if (!overridePath.isEmpty()) {
        return overridePath;
    }
    return ConfigFile().configPath() + QStringLiteral("singerie.ini");
}

void SingerieSettings::setFilePathForTests(const QString &path)
{
    QMutexLocker locker(&mutex);
    overridePath = path;
    loadedPath.clear();
}

qint64 SingerieSettings::uploadLimitKBps()
{
    return current().uploadLimitKBps;
}

qint64 SingerieSettings::downloadLimitKBps()
{
    return current().downloadLimitKBps;
}

qint64 SingerieSettings::keepFreeBytes()
{
    return current().keepFreeBytes;
}

int SingerieSettings::transfersParallel()
{
    // 8 by default: a render's frames are many files of a few MB, where the time goes in each request's round trip,
    // not in the bytes (round 3's bench: docs/dev/nextcloud-client.md › Speed).
    const auto parallel = current().transfersParallel;
    return parallel >= 1 && parallel <= 32 ? static_cast<int>(parallel) : 8;
}

bool SingerieSettings::bulkUpload()
{
    return current().bulkUpload;
}

int SingerieSettings::prefetchParallel()
{
    // Low priority: a few at once, so the member's own transfers and opens keep most of the line.
    const auto parallel = current().prefetchParallel;
    return parallel >= 1 && parallel <= 16 ? static_cast<int>(parallel) : 3;
}

bool SingerieSettings::syncPaused()
{
    return current().syncPaused;
}

int SingerieSettings::binRetentionDays()
{
    const auto days = current().binRetentionDays;
    return days >= 1 && days <= 365 ? static_cast<int>(days) : 30;
}

qint64 SingerieSettings::binMaxBytes()
{
    return std::max<qint64>(0, current().binMaxBytes);
}

} // namespace OCC
