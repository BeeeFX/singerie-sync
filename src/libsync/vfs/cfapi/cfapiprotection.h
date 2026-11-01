/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "cfapiexport.h"

#include <QString>

#include <optional>

namespace OCC
{

class SyncJournalDb;

/**
 * Answers Windows' "may this placeholder be deleted / renamed?" questions (the Cloud Files API's
 * NOTIFY_DELETE and NOTIFY_RENAME callbacks) from what the server allows for each item: the
 * remote permissions the sync journal keeps (`oc:permissions`).
 *
 * Without it, a local delete or rename the server doesn't allow goes through on the PC, the next
 * sync finds the server refusing it and puts the item back (download, or a copy for a move). With
 * it, the delete or rename is refused on the spot, the way the server would refuse it.
 *
 * The rules mirror the sync engine's own checks (ProcessDirectoryJob::checkMovePermissions):
 *  - delete: the item's CanDelete; for a folder, also every item below it that the journal knows;
 *  - rename in the same folder: the item's CanRename;
 *  - move to another folder of the sync folder: the item's CanMove and the destination folder's
 *    CanAddFile (a file) or CanAddSubDirectories (a folder);
 *  - move out of the sync folder (the Recycle Bin, another folder of the PC): as a delete.
 * Anything the journal doesn't know (a new local item, unknown permissions) is allowed: the
 * check only ever refuses what the server is known to refuse.
 *
 * Every function only reads the journal (thread-safe, its own mutex) and never waits for the
 * client's main thread: the callbacks run on the Cloud Files API's threads while the deleting
 * application waits for the answer.
 */
namespace CfApiProtection
{

struct Decision {
    bool allowed = true;
    /// Why it was refused: the item (relative path) and the missing permission, for the log.
    QString reason;
};

/// `fullPath`'s path relative to `syncRootPath` with '/' separators ("" for the root itself),
/// compared without case as Windows does; nullopt when it isn't inside the sync folder.
NEXTCLOUD_CFAPI_EXPORT std::optional<QString> relativePath(const QString &syncRootPath, const QString &fullPath);

/// May the item at `relativePath` (and, for a folder, everything below it) be deleted?
NEXTCLOUD_CFAPI_EXPORT Decision mayDelete(SyncJournalDb &journal, const QString &relativePath, bool isDirectory);

/// May the item at `relativePath` be renamed or moved to `relativeTarget`? nullopt target: moved
/// out of the sync folder (handled as a delete).
NEXTCLOUD_CFAPI_EXPORT Decision mayRename(SyncJournalDb &journal, const QString &relativePath, const std::optional<QString> &relativeTarget, bool isDirectory);

} // namespace CfApiProtection
} // namespace OCC
