/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "owncloudlib.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>
#include <optional>

namespace OCC
{

class SyncJournalFileRecord;

/**
 * The local bin of one sync folder (Singerie's client).
 *
 * When the server deletes a file this PC holds in full (never a placeholder, never one of the member's own deletes),
 * the client moves it here instead of deleting it: `<parent of the sync folder>/.singerie-bin/<root id>/<path in the
 * sync folder>`, on the same drive (a rename, nothing copied), outside the sync folder, the folder's own access rules
 * copied onto the bin. When the file comes back on the server unchanged, it is moved back instead of downloaded.
 *
 * An index beside the files (`.singerie-index.jsonl`, one JSON object per line, appended; rewritten when cleaned)
 * keeps each file's path, size, modification time, checksum, ETag, file id and when it was deleted. A folder deleted
 * on the server is one bin item holding each of its full files.
 *
 * Kept `[bin] retentionDays` days (default 30) and, only when `[bin] maxBytes` is set, the oldest out past that size
 * (singerie.ini, read live: SingerieSettings).
 */
class OWNCLOUDSYNC_EXPORT SingerieBin
{
public:
    struct File {
        QString id; ///< unique
        QString item; ///< the bin item it belongs to (its own id for a file deleted alone)
        QString path; ///< in the sync folder, with '/'
        QString stored; ///< in the bin's folder, with '/'
        qint64 size = 0;
        qint64 mtime = 0;
        QByteArray checksum; ///< the journal's checksum header ("SHA1:…"), maybe empty
        QByteArray etag;
        QByteArray fileId;
        qint64 deletedAt = 0;
    };

    struct Item {
        QString id;
        QString path; ///< the file's or the folder's path in the sync folder
        qint64 bytes = 0;
        qint64 deletedAt = 0;
        bool isFolder = false;
        int files = 0;
    };

    struct Summary {
        qint64 bytes = 0;
        int count = 0; ///< items
        qint64 oldest = 0; ///< 0: empty
    };

    enum class RestoreResult {
        Restored,
        Exists, ///< something is already at that path
        Gone, ///< no such item (or its files are missing)
        NotAllowed, ///< the member may not create it there
        Io,
    };

    /// `syncRoot`: the sync folder (absolute); `rootId`: its id (the journal's hash, rootIdFromJournalPath()).
    SingerieBin(const QString &syncRoot, const QString &rootId);

    /// `.sync_<12 hex>.db` → `<12 hex>`; another name: its name without the extension.
    static QString rootIdFromJournalPath(const QString &journalPath);
    /// `<parent of syncRoot>/.singerie-bin/<rootId>`; empty when the sync folder has no parent (a drive's root).
    static QString folderFor(const QString &syncRoot, const QString &rootId);

    [[nodiscard]] bool isUsable() const
    {
        return !_folder.isEmpty();
    }
    [[nodiscard]] QString folder() const
    {
        return _folder;
    }
    [[nodiscard]] QString syncRoot() const
    {
        return _syncRoot;
    }

    /// Moves the sync folder's file at `path` into the bin (one item). `record`: the journal's values for it.
    bool takeFile(const QString &path, const SyncJournalFileRecord &record, QString *error);
    /// Moves a folder's full files into the bin as one item. `files`: each file's path in the sync folder and its
    /// journal record (maybe invalid: a file the journal doesn't know is kept too).
    bool takeFolder(const QString &folderPath, const QList<QPair<QString, SyncJournalFileRecord>> &files, QString *error);

    /**
     * The newest bin file that is the server's file described, or nothing:
     * - the same content: the server's checksum equal to the file's, and the same size (at that path, or anywhere
     *   for a file moved meanwhile);
     * - without a checksum from the server: the same path, size and modification time, and the same ETag or file id.
     */
    std::optional<File>
    findMatch(const QString &path, qint64 size, qint64 mtime, const QByteArray &checksumHeader, const QByteArray &etag, const QByteArray &fileId);
    /// Moves a bin file to `destination` (absolute, same drive) and forgets it.
    bool moveOut(const File &file, const QString &destination, QString *error);

    Summary summary();
    /// Items, newest first.
    QList<Item> items(int offset, int limit, int *total);
    /// Puts an item back in the sync folder at its path (the next sync uploads what the server no longer has).
    /// `mayCreate(path)`: whether the member may create that path (the server's permissions on its parent).
    RestoreResult restore(const QString &itemId, QString *where, const std::function<bool(const QString &)> &mayCreate);
    /// Deletes everything in the bin: (bytes, items).
    QPair<qint64, int> empty();
    /// Deletes the items deleted more than `retentionDays` ago, then, when `maxBytes` > 0, the oldest while the bin
    /// holds more: (bytes, items) deleted.
    QPair<qint64, int> clean(int retentionDays, qint64 maxBytes, qint64 now);
    /// clean() with singerie.ini's values at most once an hour (and at the first call).
    void cleanIfDue();

    /// Copies `from`'s access rules onto `to`, protected (not inherited from `to`'s parent): Windows only (elsewhere
    /// nothing, true).
    static bool copyProtectedDacl(const QString &from, const QString &to, QString *error);

    /// empty() and clean() take files out of the index at once and delete them on a thread of their own (SYNC-10: a
    /// big bin no longer stops the client's only working thread); this waits for those deletions (tests, exit).
    static void waitForDeletes();

private:
    void load();
    void appendLine(const QByteArray &line);
    void rewriteIndex();
    bool ensureFolder(QString *error);
    QString freeStoredPath(const QString &path) const;
    bool moveIn(const QString &path, const QString &stored, QString *error);
    /// Forgets an item's files; their stored paths (absolute) are added to `toDelete` when given.
    void forgetItem(const QString &itemId, QStringList *toDelete);
    QString newId();
    void remember(const File &file);
    void forgetFile(const QString &id);
    /// Files in the bin's folder that no index entry names (the client stopped between the move and the index
    /// line): taken in as items found now, so they are kept and cleaned like the others (SYNC-11).
    void sweepOrphans(qint64 now);
    /// What the journal knows of a file, kept only when the file on the PC is still what the journal describes
    /// (SYNC-11): a file changed since keeps no checksum, ETag or file id.
    static void describe(File &file, const SyncJournalFileRecord &record);

    QString _syncRoot;
    QString _rootId;
    QString _folder;
    bool _loaded = false;
    QHash<QString, File> _files; ///< by id
    QMultiHash<qint64, QString> _bySize; ///< file ids by size: findMatch looks at same-size files only (SYNC-10)
    std::optional<QList<Item>> _items; ///< every item, newest first, until the index changes
    qint64 _lastClean = 0;
    qint64 _lastOrphanSweep = 0;
    bool _sweepFolders = false; ///< files were deleted: the folders they emptied go at the next clean
    void removeEmptyFolders();
    int _counter = 0;
};

} // namespace OCC
