/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>

namespace OCC
{

class Folder;

/**
 * Singerie's pre-download (SINGERIE_PREFETCH): the files a member is about to need (a taken task's shot: its work
 * area's scenes and the published versions they load, as singerie-core lists them) brought onto the PC in the
 * background, before they are opened.
 *
 * - Low priority: a few files at once (`[prefetch] parallel`, 3), and it steps aside while the member's own sync runs,
 *   while another program reads an online-only file (SingerieOwnReads counts those), and while Singerie's pause is on.
 * - Never below the keep-free line (`[space] keepFreeBytes`, else 1 GB): it stops and says `disk_space`.
 * - Nothing pinned: the files are fetched as an open would fetch them, "Free up space" still works, and nothing new
 *   comes down by itself later. Only what was asked, never a whole project.
 * - Files and folders inside one sync folder; a folder means everything below it; files already on the PC count as
 *   done at once (so "1.2 / 3.4 GB" says how much of the shot is ready).
 *
 * The reads run on worker threads of their own and are announced (SingerieOwnReads::Scope): the virtual-files
 * callbacks refuse every other read the client asks of itself.
 */
class SingeriePrefetch : public QObject
{
    Q_OBJECT
public:
    static SingeriePrefetch *instance();
    ~SingeriePrefetch() override;

    /// `{"id"?, "label"?, "paths": [...]}` → `{"ok": true, "id"}` or `{"ok": false, "error", "message"}`.
    QJsonObject start(const QJsonObject &request);
    /// `{"ok": true}` or `{"ok": false, "error": "not_found"}`.
    QJsonObject cancel(const QString &id);
    /// The pre-downloads running or finished in the last minutes, for SINGERIE_STATUS.
    [[nodiscard]] QJsonArray status() const;

    struct Job;

Q_SIGNALS:
    /// `{"type": "prefetch", "id", "label", "state", "reason", "files", "files_done", "bytes", "bytes_done", "failed"}`:
    /// at most once a second while it moves, and at once when its state changes.
    void event(const QJsonObject &event);

private:
    explicit SingeriePrefetch(QObject *parent = nullptr);
    void tick();
    void runJob(const std::shared_ptr<Job> &job);
    void fetchFiles(const std::shared_ptr<Job> &job);

    QThreadPool *_pool; ///< never deleted: a fetch still running at exit waits on the main thread
    QTimer _timer;
    mutable QMutex _mutex;
    QList<std::shared_ptr<Job>> _jobs;
    std::atomic<bool> _foldersBusy{false};
    int _counter = 0;
};

} // namespace OCC
