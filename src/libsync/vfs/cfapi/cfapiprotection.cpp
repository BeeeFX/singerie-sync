/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cfapiprotection.h"

#include "common/remotepermissions.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"

#include <QDir>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcCfApiProtection, "nextcloud.sync.vfs.cfapi.protection", QtInfoMsg)

namespace OCC::CfApiProtection
{

namespace
{

Decision allowed()
{
    return {};
}

Decision refused(const QString &path, const char *missing)
{
    return {false, QStringLiteral("%1: the server gives no %2").arg(path, QString::fromLatin1(missing))};
}

/// The journal's record for `path`, nullopt when it has none (a new local item) or can't be read.
std::optional<SyncJournalFileRecord> recordFor(SyncJournalDb &journal, const QString &path)
{
    if (path.isEmpty()) {
        return std::nullopt; // the sync folder itself has no record
    }
    SyncJournalFileRecord record;
    if (!journal.getFileRecord(path.toUtf8(), &record) || !record.isValid()) {
        return std::nullopt;
    }
    return record;
}

/// Known permissions without `permission`: the only case that refuses.
bool knownWithout(const SyncJournalFileRecord &record, RemotePermissions::Permissions permission)
{
    return !record._remotePerm.isNull() && !record._remotePerm.hasPermission(permission);
}

QString parentOf(const QString &path)
{
    const auto slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString{} : path.left(slash);
}

} // namespace

namespace
{
/// The folder `path` really is (a junction or a link followed), as Windows names what its callbacks ask about;
/// empty when it can't be read. Cached: the configured sync folder doesn't move while the client runs.
QString finalPathOf(const QString &path)
{
#ifdef Q_OS_WIN
    static QMutex mutex;
    static QHash<QString, QString> known;
    QMutexLocker locker(&mutex);
    if (const auto found = known.constFind(path); found != known.cend()) {
        return *found;
    }
    QString resolved;
    const auto handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()),
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        wchar_t buffer[32768];
        const auto length = GetFinalPathNameByHandleW(handle, buffer, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(handle);
        if (length > 0 && length < 32768) {
            resolved = QString::fromWCharArray(buffer, static_cast<int>(length));
            if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
                resolved = QStringLiteral("\\\\") + resolved.mid(8);
            } else if (resolved.startsWith(QStringLiteral("\\\\?\\"))) {
                resolved = resolved.mid(4);
            }
        }
    }
    known.insert(path, resolved);
    return resolved;
#else
    Q_UNUSED(path)
    return {};
#endif
}

std::optional<QString> relativeTo(const QString &syncRootPath, const QString &fullPath)
{
    auto root = QDir::fromNativeSeparators(syncRootPath);
    while (root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    auto path = QDir::fromNativeSeparators(fullPath);
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path.compare(root, Qt::CaseInsensitive) == 0) {
        return QString{};
    }
    if (root.isEmpty() || !path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    return path.mid(root.size() + 1);
}
} // namespace

std::optional<QString> relativePath(const QString &syncRootPath, const QString &fullPath)
{
    if (auto direct = relativeTo(syncRootPath, fullPath)) {
        return direct;
    }
    // The sync folder reached through a junction (SYNC-17): Windows names the real folder in its callbacks.
    const auto real = finalPathOf(syncRootPath);
    if (real.isEmpty() || QDir::cleanPath(QDir::fromNativeSeparators(real)).compare(QDir::cleanPath(QDir::fromNativeSeparators(syncRootPath)), Qt::CaseInsensitive) == 0) {
        return std::nullopt;
    }
    return relativeTo(real, fullPath);
}

Decision mayDelete(SyncJournalDb &journal, const QString &relativePath, bool isDirectory)
{
    if (relativePath.isEmpty()) {
        return allowed(); // not ours to answer: the sync folder itself isn't a placeholder
    }
    if (const auto record = recordFor(journal, relativePath)) {
        if (knownWithout(*record, RemotePermissions::CanDelete)) {
            return refused(relativePath, "delete permission (D)");
        }
    }
    if (!isDirectory) {
        return allowed();
    }
    // A folder goes with everything in it: refused when anything below may not be deleted.
    auto decision = allowed();
    const auto readOk = journal.getFilesBelowPath(relativePath.toUtf8(), [&decision](const SyncJournalFileRecord &below) {
        if (decision.allowed && knownWithout(below, RemotePermissions::CanDelete)) {
            decision = refused(QString::fromUtf8(below._path), "delete permission (D) for an item inside");
        }
    });
    if (!readOk) {
        qCWarning(lcCfApiProtection) << "Could not read the journal below" << relativePath << ": allowing the delete";
        return allowed();
    }
    return decision;
}

Decision mayRename(SyncJournalDb &journal, const QString &relativePath, const std::optional<QString> &relativeTarget, bool isDirectory)
{
    if (relativePath.isEmpty()) {
        return allowed();
    }
    if (!relativeTarget) {
        // Out of the sync folder (the Recycle Bin, another folder): for the server, a delete.
        return mayDelete(journal, relativePath, isDirectory);
    }
    const auto record = recordFor(journal, relativePath);
    const auto isRename = parentOf(relativePath).compare(parentOf(*relativeTarget), Qt::CaseInsensitive) == 0;
    if (isRename) {
        if (record && knownWithout(*record, RemotePermissions::CanRename)) {
            return refused(relativePath, "rename permission (N)");
        }
        return allowed();
    }
    if (record && knownWithout(*record, RemotePermissions::CanMove)) {
        return refused(relativePath, "move permission (V)");
    }
    if (const auto destination = recordFor(journal, parentOf(*relativeTarget))) {
        if (isDirectory && knownWithout(*destination, RemotePermissions::CanAddSubDirectories)) {
            return refused(parentOf(*relativeTarget), "permission to add folders (K)");
        }
        if (!isDirectory && knownWithout(*destination, RemotePermissions::CanAddFile)) {
            return refused(parentOf(*relativeTarget), "permission to add files (C)");
        }
    }
    return allowed();
}

} // namespace OCC::CfApiProtection
