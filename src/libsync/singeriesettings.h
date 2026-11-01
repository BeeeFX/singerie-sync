/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "owncloudlib.h"

#include <QString>

namespace OCC
{

/**
 * Settings written by the application that runs this client (Singerie) in `singerie.ini` beside the client's own
 * configuration file, and re-read whenever that file changes: they take effect at the next sync or the next transfer,
 * without restarting the client. The user can't change them in the client (it shows no settings window).
 *
 *   [transfers] uploadLimitKBps, downloadLimitKBps   0 = no limit; absent = the account's own setting
 *   [space]     keepFreeBytes                        downloads stop below this much free space; absent = the
 *                                                    OWNCLOUD_FREE_SPACE_BYTES variable or the client's 1 GB
 *
 * Each value is read on demand (a cheap check of the file's modification time), so callers ask every time.
 */
class OWNCLOUDSYNC_EXPORT SingerieSettings
{
public:
    /// The file read: `<config folder>/singerie.ini`, or `path` when set (tests).
    static QString filePath();
    static void setFilePathForTests(const QString &path);

    /// -1 when the file doesn't set it.
    static qint64 uploadLimitKBps();
    static qint64 downloadLimitKBps();
    static qint64 keepFreeBytes();
};

} // namespace OCC
