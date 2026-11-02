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
 *               parallel                             1–32, absent = 8: files moving at once (a render's frames)
 *               bulkUpload                           true/false, absent = false: small files several per request
 *   [space]     keepFreeBytes                        downloads stop below this much free space; absent = the
 *                                                    OWNCLOUD_FREE_SPACE_BYTES variable or the client's 1 GB
 *   [prefetch]  parallel                             1–16, absent = 3: files Singerie's pre-download fetches at once
 *   [sync]      paused                               true/false, absent = false: no sync runs (the running one stops)
 *   [bin]       retentionDays                        1–365, absent = 30: how long the local bin keeps a file
 *               maxBytes                             0 or absent = no cap; else the oldest go past that size
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

    /// `[transfers] parallel`: how many files move at once (1–32; 8 when absent or out of range).
    static int transfersParallel();
    /// `[transfers] bulkUpload`: small, settled files go up several per request where the server offers it.
    static bool bulkUpload();

    /// `[prefetch] parallel`: files the pre-download (SINGERIE_PREFETCH) fetches at once (1–16; 3 when absent).
    static int prefetchParallel();

    /// `[sync] paused`: Singerie's Pause (Settings › Files). Replaces the folder's own `paused` key (A27).
    static bool syncPaused();

    /// The local bin's retention (days, 1–365; 30 when absent or out of range) and size cap (bytes, 0 = none).
    static int binRetentionDays();
    static qint64 binMaxBytes();
};

} // namespace OCC
