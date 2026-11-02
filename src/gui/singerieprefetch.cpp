/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singerieprefetch.h"

#include "common/utility.h"
#include "filesystem.h"
#include "folder.h"
#include "folderman.h"
#include "singerieownreads.h"
#include "singeriesettings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QThread>

#ifdef Q_OS_WIN
// clang-format off: windows.h first, cfapi.h needs its types
#include <windows.h>

#include <cfapi.h>
// clang-format on
#endif

#include <algorithm>
#include <vector>

Q_LOGGING_CATEGORY(lcSingeriePrefetch, "nextcloud.gui.singerie.prefetch", QtInfoMsg)

namespace OCC
{

struct SingeriePrefetch::Job {
    QString id;
    QString label;
    QString root; ///< the sync folder, '/' separators
    QStringList paths; ///< native separators
    QString state = QStringLiteral("listing");
    QString reason;
    QList<QPair<QString, qint64>> todo;
    int next = 0;
    int active = 0;
    int files = 0;
    int filesDone = 0;
    int failed = 0;
    QStringList unreachable; ///< folders and files it couldn't list or find (the first 20), native separators
    qint64 bytes = 0;
    qint64 bytesDone = 0;
    qint64 finishedAt = 0;
    QByteArray lastSent;
    qint64 lastSentAt = 0;
    std::atomic<bool> cancelled{false};
};

namespace
{

constexpr auto defaultKeepFree = 1000 * 1000 * 1000LL;

bool isFinished(const QString &state)
{
    return state == QLatin1String("done") || state == QLatin1String("stopped") || state == QLatin1String("cancelled");
}

/// The folders from the sync folder down to `path`'s parent, announced so that listing them from here is let through.
std::vector<std::unique_ptr<SingerieOwnReads::Scope>> announceParents(const QString &root, const QString &path)
{
    std::vector<std::unique_ptr<SingerieOwnReads::Scope>> scopes;
    auto current = QDir::cleanPath(QDir::fromNativeSeparators(path));
    while (current.length() > root.length()) {
        current = QFileInfo(current).path();
        scopes.push_back(std::make_unique<SingerieOwnReads::Scope>(current));
    }
    return scopes;
}

#ifdef Q_OS_WIN
/// Every path handed to Windows in its long form (SYNC-7): a shot's folders pass 260 characters.
std::wstring winPath(const QString &path)
{
    return FileSystem::longWinPath(QDir::toNativeSeparators(path)).toStdWString();
}

bool isOnlineOnly(const QString &path)
{
    const auto attributes = GetFileAttributesW(winPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE));
}

bool hydrate(const QString &path, QString *error)
{
    const SingerieOwnReads::Scope announced(path);
    const auto handle = CreateFileW(winPath(path).c_str(),
                                    FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = Utility::formatWinError(GetLastError());
        return false;
    }
    LARGE_INTEGER offset = {};
    LARGE_INTEGER length;
    length.QuadPart = -1; // to the end of the file
    const auto result = CfHydratePlaceholder(handle, offset, length, CF_HYDRATE_FLAG_NONE, nullptr);
    CloseHandle(handle);
    if (FAILED(result)) {
        *error = Utility::formatWinError(result);
        return false;
    }
    return true;
}
#else
bool isOnlineOnly(const QString &)
{
    return false;
}

bool hydrate(const QString &, QString *error)
{
    *error = QStringLiteral("virtual files are Windows-only here");
    return false;
}
#endif

#ifdef Q_OS_WIN
/// One folder's entries straight from Windows (Qt's file info takes a cloud-files placeholder for a link): name, is a
/// folder, size, online-only. Real links and junctions are left out.
struct Entry {
    QString path;
    bool folder = false;
    qint64 size = 0;
    bool onlineOnly = false;
};

/// `ok` false: the folder couldn't be listed (counted as failed, never as "nothing to get").
QList<Entry> entries(const QString &folder, bool *ok)
{
    QList<Entry> found;
    const auto pattern = winPath(folder) + L"\\*";
    WIN32_FIND_DATAW data;
    const auto handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        qCWarning(lcSingeriePrefetch) << "Couldn't list" << folder << Utility::formatWinError(GetLastError());
        *ok = false;
        return found;
    }
    *ok = true;
    do {
        const auto name = QString::fromWCharArray(data.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String("..")) {
            continue;
        }
        const auto reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (reparse && (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
            continue;
        }
        Entry entry;
        entry.path = folder + QLatin1Char('/') + name;
        entry.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entry.onlineOnly = (data.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE)) != 0;
        found.append(entry);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    // Names in order: a sequence's frames come down first frame first.
    std::sort(found.begin(), found.end(), [](const Entry &a, const Entry &b) {
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });
    return found;
}

void walkFolder(const QString &folder,
                QList<QPair<QString, qint64>> &out,
                int &already,
                qint64 &alreadyBytes,
                QStringList &unreachable,
                const std::atomic<bool> &cancelled)
{
    const SingerieOwnReads::Scope announced(folder);
    auto ok = false;
    const auto found = entries(folder, &ok);
    if (!ok) {
        unreachable.append(QDir::toNativeSeparators(folder));
        return;
    }
    for (const auto &entry : found) {
        if (cancelled) {
            return;
        }
        if (entry.folder) {
            walkFolder(entry.path, out, already, alreadyBytes, unreachable, cancelled);
        } else if (entry.onlineOnly) {
            out.append({entry.path, entry.size});
        } else {
            ++already;
            alreadyBytes += entry.size;
        }
    }
}
#endif

void walk(const QString &path,
          QList<QPair<QString, qint64>> &out,
          int &already,
          qint64 &alreadyBytes,
          QStringList &unreachable,
          const std::atomic<bool> &cancelled)
{
#ifdef Q_OS_WIN
    if (cancelled) {
        return;
    }
    const auto attributes = GetFileAttributesW(winPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        qCWarning(lcSingeriePrefetch) << "Nothing to get ready at" << path << Utility::formatWinError(GetLastError());
        unreachable.append(QDir::toNativeSeparators(path));
        return;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        walkFolder(path, out, already, alreadyBytes, unreachable, cancelled);
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    const auto size = GetFileAttributesExW(winPath(path).c_str(), GetFileExInfoStandard, &info)
        ? (static_cast<qint64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow
        : 0;
    if (isOnlineOnly(path)) {
        out.append({path, size});
    } else {
        ++already;
        alreadyBytes += size;
    }
#else
    Q_UNUSED(path)
    Q_UNUSED(out)
    Q_UNUSED(already)
    Q_UNUSED(alreadyBytes)
    Q_UNUSED(unreachable)
    Q_UNUSED(cancelled)
#endif
}

} // namespace

SingeriePrefetch *SingeriePrefetch::instance()
{
    // Never deleted, like its pool: the process ends with it.
    static auto *prefetch = new SingeriePrefetch;
    return prefetch;
}

SingeriePrefetch::SingeriePrefetch(QObject *parent)
    : QObject(parent)
    , _pool(new QThreadPool)
{
    _timer.setInterval(1000);
    connect(&_timer, &QTimer::timeout, this, &SingeriePrefetch::tick);
    _timer.start();
}

SingeriePrefetch::~SingeriePrefetch()
{
    QMutexLocker locker(&_mutex);
    for (const auto &job : std::as_const(_jobs)) {
        job->cancelled = true;
    }
}

QJsonObject SingeriePrefetch::start(const QJsonObject &request)
{
    const auto refuse = [](const QString &error, const QString &message) {
        return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}, {QStringLiteral("message"), message}};
    };
    const auto paths = request.value(QStringLiteral("paths")).toArray();
    if (paths.isEmpty()) {
        return refuse(QStringLiteral("bad_request"), QStringLiteral("No paths to get ready."));
    }
    const auto sensitivity = Utility::fsCasePreserving() ? Qt::CaseInsensitive : Qt::CaseSensitive;
    Folder *folder = nullptr;
    QStringList wanted;
    for (const auto &value : paths) {
        const auto path = QDir::cleanPath(QDir::fromNativeSeparators(value.toString().trimmed()));
        Folder *found = nullptr;
        for (const auto candidate : FolderMan::instance()->map()) {
            const auto root = QDir::cleanPath(candidate->path());
            if (path.compare(root, sensitivity) == 0 || path.startsWith(root + QLatin1Char('/'), sensitivity)) {
                found = candidate;
                break;
            }
        }
        if (!found || (folder && found != folder)) {
            return refuse(QStringLiteral("not_in_sync_folder"), QStringLiteral("Every path must be in the same sync folder."));
        }
        folder = found;
        wanted.append(path);
    }
    if (!folder->virtualFilesEnabled()) {
        return refuse(QStringLiteral("no_virtual_files"), QStringLiteral("That sync folder keeps every file on the PC already."));
    }

    auto job = std::make_shared<Job>();
    job->id = request.value(QStringLiteral("id")).toString();
    if (job->id.isEmpty()) {
        job->id = QStringLiteral("prefetch-%1-%2").arg(QDateTime::currentSecsSinceEpoch()).arg(++_counter);
    }
    job->label = request.value(QStringLiteral("label")).toString();
    job->root = QDir::cleanPath(folder->path());
    job->paths = wanted;
    {
        QMutexLocker locker(&_mutex);
        // The same id asked again replaces the earlier one (the member took the task again, or opened it).
        for (const auto &earlier : std::as_const(_jobs)) {
            if (earlier->id == job->id && !isFinished(earlier->state)) {
                earlier->cancelled = true;
            }
        }
        _jobs.append(job);
    }
    _pool->setMaxThreadCount(SingerieSettings::prefetchParallel() + 1);
    _pool->start([this, job] {
        runJob(job);
    });
    qCInfo(lcSingeriePrefetch) << "Pre-download" << job->id << job->label << "of" << wanted.size() << "paths";
    return {{QStringLiteral("ok"), true}, {QStringLiteral("id"), job->id}};
}

QJsonObject SingeriePrefetch::cancel(const QString &id)
{
    QMutexLocker locker(&_mutex);
    for (const auto &job : std::as_const(_jobs)) {
        if (job->id == id && !isFinished(job->state)) {
            job->cancelled = true;
            return {{QStringLiteral("ok"), true}};
        }
    }
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("not_found")}};
}

static QJsonObject describe(const SingeriePrefetch::Job &job)
{
    return {
        {QStringLiteral("type"), QStringLiteral("prefetch")},
        {QStringLiteral("id"), job.id},
        {QStringLiteral("label"), job.label},
        {QStringLiteral("state"), job.state},
        {QStringLiteral("reason"), job.reason.isEmpty() ? QJsonValue() : QJsonValue(job.reason)},
        {QStringLiteral("files"), job.files},
        {QStringLiteral("files_done"), job.filesDone},
        {QStringLiteral("bytes"), job.bytes},
        {QStringLiteral("bytes_done"), job.bytesDone},
        {QStringLiteral("failed"), job.failed},
        {QStringLiteral("unreachable"), QJsonArray::fromStringList(job.unreachable)},
    };
}

QJsonArray SingeriePrefetch::status() const
{
    QMutexLocker locker(&_mutex);
    QJsonArray out;
    for (const auto &job : std::as_const(_jobs)) {
        auto body = describe(*job);
        body.remove(QStringLiteral("type"));
        out.append(body);
    }
    return out;
}

void SingeriePrefetch::tick()
{
    // The member's own sync comes first: a pre-download waits while a folder syncs.
    auto busy = false;
    for (const auto folder : FolderMan::instance()->map()) {
        busy = busy || folder->isSyncRunning();
    }
    _foldersBusy = busy;

    QList<QJsonObject> events;
    {
        QMutexLocker locker(&_mutex);
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto &job : std::as_const(_jobs)) {
            const auto body = describe(*job);
            const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
            if (bytes != job->lastSent) {
                job->lastSent = bytes;
                job->lastSentAt = now;
                events.append(body);
            }
        }
        // Finished ones stay in the status for ten minutes.
        _jobs.erase(std::remove_if(_jobs.begin(),
                                   _jobs.end(),
                                   [now](const std::shared_ptr<Job> &job) {
                                       return isFinished(job->state) && job->finishedAt > 0 && now - job->finishedAt > 10 * 60 * 1000;
                                   }),
                    _jobs.end());
    }
    for (const auto &body : std::as_const(events)) {
        Q_EMIT event(body);
    }
}

void SingeriePrefetch::runJob(const std::shared_ptr<Job> &job)
{
    QList<QPair<QString, qint64>> todo;
    int already = 0;
    qint64 alreadyBytes = 0;
    QStringList unreachable;
    for (const auto &path : std::as_const(job->paths)) {
        const auto parents = announceParents(job->root, path);
        walk(path, todo, already, alreadyBytes, unreachable, job->cancelled);
    }
    {
        QMutexLocker locker(&_mutex);
        job->todo = todo;
        // What couldn't be listed or found is failed, never "done" in silence (SYNC-7).
        job->failed += static_cast<int>(unreachable.size());
        job->unreachable = unreachable.mid(0, 20);
        job->files = static_cast<int>(todo.size()) + already;
        job->filesDone = already;
        job->bytesDone = alreadyBytes;
        job->bytes = alreadyBytes;
        for (const auto &[path, size] : std::as_const(todo)) {
            job->bytes += size;
        }
        job->state = QStringLiteral("running");
    }
    qCInfo(lcSingeriePrefetch) << "Pre-download" << job->id << ":" << todo.size() << "files to fetch," << already << "already on this PC";
    const auto workers = qMax(1, qMin(SingerieSettings::prefetchParallel(), static_cast<int>(todo.size())));
    for (auto i = 1; i < workers; ++i) {
        _pool->start([this, job] {
            fetchFiles(job);
        });
    }
    fetchFiles(job);
}

void SingeriePrefetch::fetchFiles(const std::shared_ptr<Job> &job)
{
    {
        QMutexLocker locker(&_mutex);
        ++job->active;
    }
    while (true) {
        QString path;
        qint64 size = 0;
        // Step aside: Singerie's pause, the member's own sync, another program reading an online-only file.
        while (!job->cancelled) {
            QString reason;
            if (SingerieSettings::syncPaused()) {
                reason = QStringLiteral("paused");
            } else if (_foldersBusy || SingerieOwnReads::memberHydrations() > 0) {
                reason = QStringLiteral("busy");
            }
            {
                QMutexLocker locker(&_mutex);
                if (job->state == QLatin1String("running") || job->state == QLatin1String("waiting")) {
                    job->state = reason.isEmpty() ? QStringLiteral("running") : QStringLiteral("waiting");
                    job->reason = reason;
                }
            }
            if (reason.isEmpty()) {
                break;
            }
            QThread::msleep(500);
        }
        {
            QMutexLocker locker(&_mutex);
            if (job->cancelled || isFinished(job->state) || job->next >= job->todo.size()) {
                break;
            }
            std::tie(path, size) = job->todo.at(job->next++);
        }
        if (!isOnlineOnly(path)) {
            // Opened meanwhile: already here.
            QMutexLocker locker(&_mutex);
            ++job->filesDone;
            job->bytesDone += size;
            continue;
        }
        const auto keepFree = SingerieSettings::keepFreeBytes() >= 0 ? SingerieSettings::keepFreeBytes() : defaultKeepFree;
        {
            // The space is reserved before the fetch (SYNC-7): workers fetching side by side can't all pass the check
            // and end under the keep-free line together.
            QMutexLocker locker(&_mutex);
            const auto free = Utility::freeDiskSpace(job->root);
            if (free >= 0 && free - _reserved - size < keepFree) {
                qCWarning(lcSingeriePrefetch) << "Pre-download" << job->id << "stops: the disk would go under its keep-free line" << free << _reserved
                                              << size << keepFree;
                job->state = QStringLiteral("stopped");
                job->reason = QStringLiteral("disk_space");
                break;
            }
            _reserved += size;
        }
        QString error;
        const auto ok = hydrate(path, &error);
        QMutexLocker locker(&_mutex);
        _reserved -= size;
        if (ok) {
            ++job->filesDone;
            job->bytesDone += size;
        } else {
            ++job->failed;
            qCWarning(lcSingeriePrefetch) << "Pre-download" << job->id << "couldn't fetch" << path << error;
        }
    }
    QMutexLocker locker(&_mutex);
    if (--job->active == 0 && !isFinished(job->state)) {
        job->state = job->cancelled ? QStringLiteral("cancelled") : QStringLiteral("done");
        job->reason = job->failed > 0 && !job->cancelled ? QStringLiteral("some_failed") : QString();
    }
    if (job->active == 0 && job->finishedAt == 0) {
        job->finishedAt = QDateTime::currentMSecsSinceEpoch();
        qCInfo(lcSingeriePrefetch) << "Pre-download" << job->id << job->state << job->filesDone << "of" << job->files << "files," << job->failed << "failed";
    }
}

} // namespace OCC
