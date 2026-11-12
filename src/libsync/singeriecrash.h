/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "owncloudlib.h"

#include <QString>
#include <QStringList>

namespace OCC {

/**
 * Singerie Sync shows no window, so a crash leaves nothing behind (Singerie's SYNC-M2). When the member ticks "Keep a
 * crash file for the developers" in Singerie (Settings › Files), Singerie writes `[diagnostics] crashDumps=true` in
 * singerie.ini and the client keeps a small crash file (a Windows minidump: the threads' stacks, no file contents) of
 * its own crash in `<config folder>/crashes/`, three at most, on this PC only: nothing ever sends it anywhere.
 */
class OWNCLOUDSYNC_EXPORT SingerieCrash
{
public:
    /// The folder the crash files go to: `<config folder>/crashes`.
    static QString folder();
    /// Reads singerie.ini once and, when it asks for crash files, makes the folder, keeps the two newest of what is
    /// there (the next crash makes three) and sets the handler. Windows only; elsewhere nothing. Returns whether set.
    static bool install();
    /// The `count` newest `.dmp` files of `folder` kept, the others deleted: the names deleted.
    static QStringList prune(const QString &folder, int count);
};

} // namespace OCC
