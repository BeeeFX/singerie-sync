/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singerieownreads.h"

#include <QDir>
#include <QHash>
#include <QMutex>

#include <atomic>

namespace OCC
{

namespace
{

QMutex mutex;
QHash<QString, int> announced;
std::atomic<int> memberCount{0};

QString keyOf(const QString &path)
{
    // Windows' callbacks give `C:\folder\file`; the paths are compared whatever their case or separators.
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

} // namespace

SingerieOwnReads::Scope::Scope(const QString &path)
    : _key(keyOf(path))
{
    QMutexLocker locker(&mutex);
    ++announced[_key];
}

SingerieOwnReads::Scope::~Scope()
{
    QMutexLocker locker(&mutex);
    if (--announced[_key] <= 0) {
        announced.remove(_key);
    }
}

bool SingerieOwnReads::allowed(const QString &path)
{
    QMutexLocker locker(&mutex);
    return announced.contains(keyOf(path));
}

void SingerieOwnReads::memberHydrationStarted()
{
    ++memberCount;
}

void SingerieOwnReads::memberHydrationFinished()
{
    --memberCount;
}

int SingerieOwnReads::memberHydrations()
{
    return memberCount.load();
}

} // namespace OCC
