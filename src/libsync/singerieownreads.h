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
 * The client's own reads of its placeholders, for Singerie's pre-download (SINGERIE_PREFETCH).
 *
 * Windows asks the sync app for a placeholder's data (or a folder's listing) whoever reads it, the sync app too.
 * Upstream refuses those asked by the client itself: a read from the client's main thread would wait for the main
 * thread forever. The pre-download reads from worker threads of its own, so it says beforehand which path it is about
 * to read (`Scope`), and the callbacks let exactly those through.
 *
 * It also counts the hydrations other programs asked for (the member opening a file), so the pre-download can step
 * aside while they run.
 */
class OWNCLOUDSYNC_EXPORT SingerieOwnReads
{
public:
    /// While it lives, the client's own reads of `path` (a file, or a folder's listing) are let through.
    class OWNCLOUDSYNC_EXPORT Scope
    {
    public:
        explicit Scope(const QString &path);
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        QString _key;
    };

    /// Whether the client's own read of `path` was announced by a `Scope`.
    static bool allowed(const QString &path);

    /// Hydrations asked for by other programs, running now.
    static void memberHydrationStarted();
    static void memberHydrationFinished();
    static int memberHydrations();
};

} // namespace OCC
