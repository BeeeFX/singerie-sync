/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singeriebin.h"

#include "common/filesystembase.h"
#include "common/syncjournalfilerecord.h"
#include "filesystem.h"
#include "singeriesettings.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>

#include <algorithm>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcSingerieBin, "nextcloud.sync.singeriebin", QtInfoMsg)

namespace OCC
{

namespace
{

const auto indexName = QStringLiteral(".singerie-index.jsonl");

QJsonObject toJson(const SingerieBin::File &file)
{
    return {
        {QStringLiteral("id"), file.id},
        {QStringLiteral("item"), file.item},
        {QStringLiteral("path"), file.path},
        {QStringLiteral("stored"), file.stored},
        {QStringLiteral("size"), file.size},
        {QStringLiteral("mtime"), file.mtime},
        {QStringLiteral("checksum"), QString::fromUtf8(file.checksum)},
        {QStringLiteral("etag"), QString::fromUtf8(file.etag)},
        {QStringLiteral("fileid"), QString::fromUtf8(file.fileId)},
        {QStringLiteral("deleted"), file.deletedAt},
    };
}

SingerieBin::File fromJson(const QJsonObject &object)
{
    SingerieBin::File file;
    file.id = object.value(QStringLiteral("id")).toString();
    file.item = object.value(QStringLiteral("item")).toString();
    file.path = object.value(QStringLiteral("path")).toString();
    file.stored = object.value(QStringLiteral("stored")).toString();
    file.size = object.value(QStringLiteral("size")).toInteger();
    file.mtime = object.value(QStringLiteral("mtime")).toInteger();
    file.checksum = object.value(QStringLiteral("checksum")).toString().toUtf8();
    file.etag = object.value(QStringLiteral("etag")).toString().toUtf8();
    file.fileId = object.value(QStringLiteral("fileid")).toString().toUtf8();
    file.deletedAt = object.value(QStringLiteral("deleted")).toInteger();
    return file;
}

#ifdef Q_OS_WIN
/// A hydrated cloud-files placeholder becomes an ordinary file before it leaves the sync folder: outside it, no
/// provider answers for it. cldapi is loaded on demand (only the virtual-files plug-in links it).
void revertPlaceholder(const QString &path)
{
    const auto nativePath = FileSystem::longWinPath(path);
    const auto wide = reinterpret_cast<const wchar_t *>(nativePath.utf16());
    const auto attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return;
    }
    using RevertFunction = HRESULT(WINAPI *)(HANDLE, int, LPOVERLAPPED);
    static const auto revert = []() -> RevertFunction {
        const auto module = LoadLibraryW(L"cldapi.dll");
        return module ? reinterpret_cast<RevertFunction>(GetProcAddress(module, "CfRevertPlaceholder")) : nullptr;
    }();
    if (!revert) {
        return;
    }
    const auto handle = CreateFileW(wide,
                                    FILE_READ_ATTRIBUTES | WRITE_DAC,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        qCWarning(lcSingerieBin) << "Could not open" << path << "to make it an ordinary file:" << GetLastError();
        return;
    }
    const auto result = revert(handle, 0, nullptr);
    CloseHandle(handle);
    if (FAILED(result)) {
        qCInfo(lcSingerieBin) << "Not a placeholder, or not reverted:" << path << Qt::hex << static_cast<quint32>(result);
    }
}
#endif

qint64 nowSeconds()
{
    return QDateTime::currentSecsSinceEpoch();
}

} // namespace

SingerieBin::SingerieBin(const QString &syncRoot, const QString &rootId)
    : _syncRoot(QDir::cleanPath(QDir::fromNativeSeparators(syncRoot)))
    , _rootId(rootId)
    , _folder(folderFor(syncRoot, rootId))
{
}

QString SingerieBin::rootIdFromJournalPath(const QString &journalPath)
{
    auto name = QFileInfo(journalPath).completeBaseName();
    if (name.startsWith(QStringLiteral(".sync_"))) {
        name = name.mid(6);
    }
    return name;
}

QString SingerieBin::folderFor(const QString &syncRoot, const QString &rootId)
{
    const auto root = QDir::cleanPath(QDir::fromNativeSeparators(syncRoot));
    const QFileInfo info(root);
    const auto parent = info.absolutePath();
    if (rootId.isEmpty() || info.fileName().isEmpty() || parent == root || QDir::cleanPath(parent) == root) {
        return {};
    }
    return QDir::cleanPath(parent + QStringLiteral("/.singerie-bin/") + rootId);
}

QString SingerieBin::newId()
{
    return QStringLiteral("%1-%2").arg(QDateTime::currentMSecsSinceEpoch(), 0, 36).arg(++_counter, 0, 36);
}

void SingerieBin::load()
{
    if (_loaded) {
        return;
    }
    _loaded = true;
    _files.clear();
    QFile index(_folder + QLatin1Char('/') + indexName);
    if (!index.open(QIODevice::ReadOnly)) {
        return;
    }
    while (!index.atEnd()) {
        const auto line = index.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const auto object = QJsonDocument::fromJson(line).object();
        const auto op = object.value(QStringLiteral("op")).toString();
        if (op == QStringLiteral("add")) {
            const auto file = fromJson(object.value(QStringLiteral("file")).toObject());
            if (!file.id.isEmpty()) {
                _files.insert(file.id, file);
            }
        } else if (op == QStringLiteral("forget")) {
            _files.remove(object.value(QStringLiteral("id")).toString());
        }
    }
}

void SingerieBin::appendLine(const QByteArray &line)
{
    QFile index(_folder + QLatin1Char('/') + indexName);
    if (!index.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qCWarning(lcSingerieBin) << "Could not write the bin's index" << index.fileName() << index.errorString();
        return;
    }
    index.write(line + '\n');
}

void SingerieBin::rewriteIndex()
{
    QSaveFile index(_folder + QLatin1Char('/') + indexName);
    if (!index.open(QIODevice::WriteOnly)) {
        qCWarning(lcSingerieBin) << "Could not rewrite the bin's index" << index.errorString();
        return;
    }
    for (const auto &file : std::as_const(_files)) {
        const QJsonObject line{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("file"), toJson(file)}};
        index.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    }
    if (!index.commit()) {
        qCWarning(lcSingerieBin) << "Could not rewrite the bin's index" << index.errorString();
    }
}

bool SingerieBin::ensureFolder(QString *error)
{
    if (QFileInfo::exists(_folder)) {
        return true;
    }
    const auto parent = QFileInfo(_folder).absolutePath(); // .singerie-bin
    const auto parentIsNew = !QFileInfo::exists(parent);
    if (!QDir().mkpath(_folder)) {
        *error = QStringLiteral("could not make the bin folder %1").arg(_folder);
        return false;
    }
    if (parentIsNew) {
        FileSystem::setFileHidden(parent, true);
    }
    // Private like the sync folder: its access rules, protected.
    QString daclError;
    if (!copyProtectedDacl(_syncRoot, _folder, &daclError)) {
        qCWarning(lcSingerieBin) << "Could not copy the sync folder's access rules onto the bin" << _folder << daclError;
        QDir(_folder).removeRecursively();
        *error = daclError;
        return false;
    }
    qCInfo(lcSingerieBin) << "Made the local bin" << _folder;
    return true;
}

QString SingerieBin::freeStoredPath(const QString &path) const
{
    auto candidate = path;
    const QFileInfo info(path);
    const auto dir = info.path() == QStringLiteral(".") ? QString() : info.path() + QLatin1Char('/');
    const auto base = info.completeBaseName();
    const auto suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 2; QFileInfo::exists(_folder + QLatin1Char('/') + candidate) || candidate == indexName; ++n) {
        candidate = QStringLiteral("%1%2 (%3)%4").arg(dir, base.isEmpty() ? info.fileName() : base).arg(n).arg(base.isEmpty() ? QString() : suffix);
    }
    return candidate;
}

bool SingerieBin::moveIn(const QString &path, const QString &stored, QString *error)
{
    const auto source = _syncRoot + QLatin1Char('/') + path;
    const auto target = _folder + QLatin1Char('/') + stored;
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        *error = QStringLiteral("could not make %1").arg(QFileInfo(target).absolutePath());
        return false;
    }
    const auto parentPermissions = FileSystem::FilePermissionsRestore{QFileInfo(source).absolutePath(), FileSystem::FolderPermissions::ReadWrite};
    // The client's read-only mark (an attribute and a deny rule) would stop the move on a drive whose folders give
    // no "delete subfolders and files" (a D: drive): lift it first.
    FileSystem::setFileReadOnly(source, false);
#ifdef Q_OS_WIN
    revertPlaceholder(source);
#endif
    if (!FileSystem::rename(source, target, error)) {
        return false;
    }
    return true;
}

bool SingerieBin::takeFile(const QString &path, const SyncJournalFileRecord &record, QString *error)
{
    if (!isUsable() || !ensureFolder(error)) {
        return false;
    }
    load();
    File file;
    file.id = newId();
    file.item = file.id;
    file.path = path;
    file.stored = freeStoredPath(path);
    const QFileInfo info(_syncRoot + QLatin1Char('/') + path);
    file.size = info.size();
    file.mtime = FileSystem::getModTime(info.absoluteFilePath());
    file.checksum = record.isValid() ? record._checksumHeader : QByteArray();
    file.etag = record.isValid() ? record._etag : QByteArray();
    file.fileId = record.isValid() ? record._fileId : QByteArray();
    file.deletedAt = nowSeconds();
    if (!moveIn(path, file.stored, error)) {
        return false;
    }
    _files.insert(file.id, file);
    appendLine(
        QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("file"), toJson(file)}}).toJson(QJsonDocument::Compact));
    qCInfo(lcSingerieBin) << "Deleted on the server, moved to the local bin:" << path << file.size << "bytes";
    return true;
}

bool SingerieBin::takeFolder(const QString &folderPath, const QList<QPair<QString, SyncJournalFileRecord>> &files, QString *error)
{
    if (files.isEmpty()) {
        return true;
    }
    if (!isUsable() || !ensureFolder(error)) {
        return false;
    }
    load();
    const auto item = newId();
    const auto storedFolder = freeStoredPath(folderPath);
    const auto now = nowSeconds();
    auto moved = 0;
    for (const auto &[path, record] : files) {
        File file;
        file.id = newId();
        file.item = item;
        file.path = path;
        file.stored = storedFolder + path.mid(folderPath.size());
        const QFileInfo info(_syncRoot + QLatin1Char('/') + path);
        file.size = info.size();
        file.mtime = FileSystem::getModTime(info.absoluteFilePath());
        file.checksum = record.isValid() ? record._checksumHeader : QByteArray();
        file.etag = record.isValid() ? record._etag : QByteArray();
        file.fileId = record.isValid() ? record._fileId : QByteArray();
        file.deletedAt = now;
        QString moveError;
        if (!moveIn(path, file.stored, &moveError)) {
            qCWarning(lcSingerieBin) << "Could not move" << path << "to the local bin:" << moveError;
            *error = moveError;
            continue;
        }
        ++moved;
        _files.insert(file.id, file);
        appendLine(
            QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("file"), toJson(file)}}).toJson(QJsonDocument::Compact));
    }
    qCInfo(lcSingerieBin) << "Folder deleted on the server, moved to the local bin:" << folderPath << moved << "files";
    return moved == files.size();
}

std::optional<SingerieBin::File>
SingerieBin::findMatch(const QString &path, qint64 size, qint64 mtime, const QByteArray &checksumHeader, const QByteArray &etag, const QByteArray &fileId)
{
    if (!isUsable()) {
        return {};
    }
    load();
    std::optional<File> best;
    for (const auto &file : std::as_const(_files)) {
        if (file.size != size) {
            continue;
        }
        const auto sameContent = !checksumHeader.isEmpty() && !file.checksum.isEmpty() && checksumHeader == file.checksum;
        const auto sameFile =
            file.path == path && file.mtime == mtime && ((!etag.isEmpty() && etag == file.etag) || (!fileId.isEmpty() && fileId == file.fileId));
        if (!sameContent && !sameFile) {
            continue;
        }
        const auto samePath = file.path == path;
        const auto bestSamePath = best && best->path == path;
        const auto better = !best || (samePath && !bestSamePath) || (samePath == bestSamePath && file.deletedAt > best->deletedAt);
        if (better) {
            best = file;
        }
    }
    if (best && !QFileInfo::exists(_folder + QLatin1Char('/') + best->stored)) {
        _files.remove(best->id);
        appendLine(
            QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("forget")}, {QStringLiteral("id"), best->id}}).toJson(QJsonDocument::Compact));
        return findMatch(path, size, mtime, checksumHeader, etag, fileId);
    }
    return best;
}

bool SingerieBin::moveOut(const File &file, const QString &destination, QString *error)
{
    load();
    const auto source = _folder + QLatin1Char('/') + file.stored;
    if (!FileSystem::rename(source, destination, error)) {
        return false;
    }
    _files.remove(file.id);
    appendLine(QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("forget")}, {QStringLiteral("id"), file.id}}).toJson(QJsonDocument::Compact));
    qCInfo(lcSingerieBin) << "Back from the local bin instead of downloaded:" << file.path << file.size << "bytes";
    return true;
}

SingerieBin::Summary SingerieBin::summary()
{
    Summary summary;
    if (!isUsable()) {
        return summary;
    }
    load();
    QSet<QString> items;
    for (const auto &file : std::as_const(_files)) {
        summary.bytes += file.size;
        items.insert(file.item);
        if (summary.oldest == 0 || file.deletedAt < summary.oldest) {
            summary.oldest = file.deletedAt;
        }
    }
    summary.count = static_cast<int>(items.size());
    return summary;
}

QList<SingerieBin::Item> SingerieBin::items(int offset, int limit, int *total)
{
    QHash<QString, Item> byId;
    if (isUsable()) {
        load();
    }
    for (const auto &file : std::as_const(_files)) {
        auto &item = byId[file.item];
        item.id = file.item;
        item.bytes += file.size;
        item.deletedAt = std::max(item.deletedAt, file.deletedAt);
        ++item.files;
        if (file.item == file.id) {
            item.path = file.path;
        } else {
            item.isFolder = true;
        }
    }
    QList<Item> list = byId.values();
    // A folder's path: the common folder of its files.
    for (auto &item : list) {
        if (!item.isFolder) {
            continue;
        }
        QString common;
        for (const auto &file : std::as_const(_files)) {
            if (file.item != item.id) {
                continue;
            }
            const auto folder = file.path.section(QLatin1Char('/'), 0, -2);
            if (common.isNull()) {
                common = folder;
                continue;
            }
            while (!common.isEmpty() && folder != common && !folder.startsWith(common + QLatin1Char('/'))) {
                common = common.section(QLatin1Char('/'), 0, -2);
            }
        }
        item.path = common;
    }
    // Newest first; within the same second, by the order they came in (an id starts with its time in ms, base 36).
    const auto order = [](const QString &id) {
        return id.section(QLatin1Char('-'), 0, 0).toLongLong(nullptr, 36) * 100000 + id.section(QLatin1Char('-'), 1, 1).toLongLong(nullptr, 36);
    };
    std::sort(list.begin(), list.end(), [&order](const Item &a, const Item &b) {
        return a.deletedAt != b.deletedAt ? a.deletedAt > b.deletedAt : order(a.id) > order(b.id);
    });
    if (total) {
        *total = static_cast<int>(list.size());
    }
    return list.mid(std::max(0, offset), limit < 0 ? -1 : limit);
}

SingerieBin::RestoreResult SingerieBin::restore(const QString &itemId, QString *where, const std::function<bool(const QString &)> &mayCreate)
{
    load();
    QList<File> files;
    for (const auto &file : std::as_const(_files)) {
        if (file.item == itemId) {
            files.append(file);
        }
    }
    if (files.isEmpty()) {
        return RestoreResult::Gone;
    }
    for (const auto &file : std::as_const(files)) {
        if (!QFileInfo::exists(_folder + QLatin1Char('/') + file.stored)) {
            return RestoreResult::Gone;
        }
        if (QFileInfo::exists(_syncRoot + QLatin1Char('/') + file.path)) {
            return RestoreResult::Exists;
        }
        if (mayCreate && !mayCreate(file.path)) {
            return RestoreResult::NotAllowed;
        }
    }
    for (const auto &file : std::as_const(files)) {
        const auto destination = _syncRoot + QLatin1Char('/') + file.path;
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            return RestoreResult::Io;
        }
        QString error;
        if (!FileSystem::rename(_folder + QLatin1Char('/') + file.stored, destination, &error)) {
            qCWarning(lcSingerieBin) << "Could not restore" << file.path << error;
            return RestoreResult::Io;
        }
        _files.remove(file.id);
        appendLine(
            QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("forget")}, {QStringLiteral("id"), file.id}}).toJson(QJsonDocument::Compact));
    }
    if (where) {
        *where = files.size() == 1 && files.first().item == files.first().id
            ? QDir::toNativeSeparators(_syncRoot + QLatin1Char('/') + files.first().path)
            : QDir::toNativeSeparators(_syncRoot + QLatin1Char('/') + files.first().path.section(QLatin1Char('/'), 0, -2));
    }
    qCInfo(lcSingerieBin) << "Restored from the local bin:" << itemId << files.size() << "files";
    return RestoreResult::Restored;
}

void SingerieBin::forgetItem(const QString &itemId, bool deleteFiles)
{
    for (auto it = _files.begin(); it != _files.end();) {
        if (it->item != itemId) {
            ++it;
            continue;
        }
        if (deleteFiles) {
            const auto stored = _folder + QLatin1Char('/') + it->stored;
            if (QFileInfo::exists(stored)) {
                FileSystem::setFileReadOnly(stored, false);
                QString error;
                if (!FileSystem::remove(stored, &error)) {
                    qCWarning(lcSingerieBin) << "Could not delete" << stored << error;
                }
            }
        }
        it = _files.erase(it);
    }
}

QPair<qint64, int> SingerieBin::empty()
{
    if (!isUsable()) {
        return {0, 0};
    }
    const auto before = summary();
    load();
    const auto all = items(0, -1, nullptr);
    for (const auto &item : all) {
        forgetItem(item.id, true);
    }
    // Leftovers (folders of restored items, files the index lost): the bin's folder holds only what the client put
    // there.
    QDirIterator it(_folder, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const auto path = it.next();
        if (it.fileName() == indexName) {
            continue;
        }
        if (QFileInfo(path).isDir()) {
            FileSystem::removeRecursively(path, {}, nullptr, {});
        } else {
            FileSystem::setFileReadOnly(path, false);
            FileSystem::remove(path);
        }
    }
    rewriteIndex();
    qCInfo(lcSingerieBin) << "Emptied the local bin" << _folder << before.bytes << "bytes," << before.count << "items";
    return {before.bytes, before.count};
}

QPair<qint64, int> SingerieBin::clean(int retentionDays, qint64 maxBytes, qint64 now)
{
    if (!isUsable() || !QFileInfo::exists(_folder)) {
        return {0, 0};
    }
    load();
    auto all = items(0, -1, nullptr); // newest first
    qint64 bytes = 0;
    for (const auto &item : std::as_const(all)) {
        bytes += item.bytes;
    }
    qint64 freedBytes = 0;
    auto freedItems = 0;
    const auto oldestKept = now - static_cast<qint64>(retentionDays) * 24 * 3600;
    while (!all.isEmpty()) {
        const auto &oldest = all.last();
        const auto tooOld = oldest.deletedAt < oldestKept;
        const auto tooBig = maxBytes > 0 && bytes > maxBytes;
        if (!tooOld && !tooBig) {
            break;
        }
        forgetItem(oldest.id, true);
        bytes -= oldest.bytes;
        freedBytes += oldest.bytes;
        ++freedItems;
        all.removeLast();
    }
    if (freedItems > 0) {
        // Folders emptied by the clean go too.
        QDirIterator dirs(_folder, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QStringList folders;
        while (dirs.hasNext()) {
            folders.append(dirs.next());
        }
        std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) {
            return a.size() > b.size();
        });
        for (const auto &folder : std::as_const(folders)) {
            QDir().rmdir(folder);
        }
        rewriteIndex();
        qCInfo(lcSingerieBin) << "Cleaned the local bin:" << freedItems << "items," << freedBytes << "bytes (kept" << retentionDays << "days, cap" << maxBytes
                              << ")";
    }
    return {freedBytes, freedItems};
}

void SingerieBin::cleanIfDue()
{
    const auto now = nowSeconds();
    if (_lastClean != 0 && now - _lastClean < 3600) {
        return;
    }
    _lastClean = now;
    clean(SingerieSettings::binRetentionDays(), SingerieSettings::binMaxBytes(), now);
}

bool SingerieBin::copyProtectedDacl(const QString &from, const QString &to, QString *error)
{
#ifdef Q_OS_WIN
    const auto fromNative = FileSystem::longWinPath(from);
    const auto toNative = FileSystem::longWinPath(to);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto result = GetNamedSecurityInfoW(reinterpret_cast<const wchar_t *>(fromNative.utf16()),
                                        SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION,
                                        nullptr,
                                        nullptr,
                                        &dacl,
                                        nullptr,
                                        &descriptor);
    if (result != ERROR_SUCCESS) {
        *error = QStringLiteral("could not read the access rules of %1 (%2)").arg(from).arg(result);
        return false;
    }
    // The same entries, made explicit: inherited ones lose their mark, so the copy stands on its own.
    ACL_SIZE_INFORMATION sizeInfo{};
    if (!dacl || !GetAclInformation(dacl, &sizeInfo, sizeof(sizeInfo), AclSizeInformation)) {
        LocalFree(descriptor);
        *error = QStringLiteral("%1 has no access rules to copy").arg(from);
        return false;
    }
    const auto size = sizeInfo.AclBytesInUse + 16;
    auto copy = static_cast<PACL>(LocalAlloc(LPTR, size));
    if (!copy || !InitializeAcl(copy, size, ACL_REVISION_DS)) {
        LocalFree(copy);
        LocalFree(descriptor);
        *error = QStringLiteral("out of memory");
        return false;
    }
    for (DWORD i = 0; i < sizeInfo.AceCount; ++i) {
        void *ace = nullptr;
        if (!GetAce(dacl, i, &ace)) {
            continue;
        }
        auto header = static_cast<PACE_HEADER>(ace);
        QByteArray bytes(static_cast<const char *>(ace), header->AceSize);
        reinterpret_cast<PACE_HEADER>(bytes.data())->AceFlags &= ~INHERITED_ACE;
        if (!AddAce(copy, ACL_REVISION_DS, MAXDWORD, bytes.data(), header->AceSize)) {
            qCWarning(lcSingerieBin) << "Could not copy an access rule onto" << to << GetLastError();
        }
    }
    result = SetNamedSecurityInfoW(const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(toNative.utf16())),
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr,
                                   nullptr,
                                   copy,
                                   nullptr);
    LocalFree(copy);
    LocalFree(descriptor);
    if (result != ERROR_SUCCESS) {
        *error = QStringLiteral("could not set the access rules of %1 (%2)").arg(to).arg(result);
        return false;
    }
    return true;
#else
    Q_UNUSED(from)
    Q_UNUSED(to)
    Q_UNUSED(error)
    return true;
#endif
}

} // namespace OCC
