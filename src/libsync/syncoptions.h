/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2017 ownCloud GmbH
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "owncloudlib.h"
#include "common/vfs.h"

#include <QRegularExpression>
#include <QSharedPointer>
#include <QString>

#include <chrono>


namespace OCC {

class SingerieBin;

/**
 * Value class containing the options given to the sync engine
 */
class OWNCLOUDSYNC_EXPORT SyncOptions
{
public:
    SyncOptions();
    ~SyncOptions();

    /** Maximum size (in Bytes) a folder can have without asking for confirmation.
     * -1 means infinite */
    qint64 _newBigFolderSizeLimit = -1;

    /** If a confirmation should be asked for external storages */
    bool _confirmExternalStorage = false;

    /** If remotely deleted files are needed to move to trash */
    bool _moveFilesToTrash = false;

    /** Mass deletions without asking anyone (a client that shows no window): a sync that would delete every file of
     * the sync folder at once is held (nothing deleted, syncHeld() emitted, the next sync tries again); any other
     * mass deletion, past deleteFilesThreshold included, follows the server. Off: the prompt as before. */
    bool _holdWhenEverythingVanishes = false;

    /** With _holdWhenEverythingVanishes: the next sync follows a server that shows the whole sync folder empty
     * instead of holding it (the managing application checked that the member really left: SINGERIE_RELEASE_HOLD). */
    bool _releaseHold = false;

    /** With _holdWhenEverythingVanishes, a local wipe (every one of the member's own files deleted on this PC at once,
     * or the whole sync folder emptied on this PC) is held: nothing deleted on the server until someone answers.
     * The answer applies to the next sync only. */
    enum class LocalWipeAnswer {
        None, ///< hold it (and say so: SyncEngine::localWipeHeld)
        DeleteEverywhere, ///< the deletes go to the server
        PutBack, ///< the deleted files come back from the server; nothing deleted on the server
    };
    LocalWipeAnswer _localWipeAnswer = LocalWipeAnswer::None;

    /** The smallest number of the member's own files whose deletion at once counts as a local wipe (fewer is an
     * ordinary delete and follows). The whole sync folder emptied on this PC is a wipe whatever its size. */
    static constexpr int localWipeMinimumFiles = 10;

    /** The local bin (Singerie's client): files this PC holds in full that the server deleted are moved there instead
     * of deleted, and moved back instead of downloaded when the server's file comes back the same. Null: deleted. */
    QSharedPointer<SingerieBin> _bin;

    /** In a conflict, the newest save (by modification time) keeps the file's name and the other becomes the
     * conflicted copy (Singerie's client). Off: this PC's file always becomes the copy (upstream). The server's file
     * keeps the name whenever the member may not write it. */
    bool _newestSaveKeepsName = false;

    /** Files changed less than this long ago go up in chunks, whatever their size, so the server only puts the file
     * in place (the final MOVE) once it is whole: a file still growing is never left truncated on the server (Singerie's
     * A10). Before the MOVE the client checks the file is still the size and time it sent. 0: upstream's way (chunks
     * only above the chunk size). */
    std::chrono::seconds _wholeUploadAge = std::chrono::seconds(0);

    /** Create a virtual file for new files instead of downloading. May not be null */
    QSharedPointer<Vfs> _vfs;

    /** The initial un-adjusted chunk size in bytes for chunked uploads, both
     * for old and new chunking algorithm, which classifies the item to be chunked
     *
     * In chunkingNG, when dynamic chunk size adjustments are done, this is the
     * starting value and is then gradually adjusted within the
     * minChunkSize / maxChunkSize bounds.
     */
    qint64 _initialChunkSize = 100LL * 1024LL * 1024LL; // 100MiB

    /** The target duration of chunk uploads for dynamic chunk sizing.
     *
     * Set to 0 it will disable dynamic chunk sizing.
     */
    std::chrono::milliseconds _targetChunkUploadDuration = std::chrono::minutes(1);

    /** The maximum number of active jobs in parallel  */
    int _parallelNetworkJobs = 6;

    static constexpr auto chunkV2MinChunkSize = 5LL * 1000LL * 1000LL; // 5 MB
    static constexpr auto chunkV2MaxChunkSize = 5LL * 1000LL * 1000LL * 1000LL; // 5 GB

    /** The minimum chunk size in bytes for chunked uploads */
    [[nodiscard]] qint64 minChunkSize() const;
    void setMinChunkSize(const qint64 minChunkSize);

    /** The maximum chunk size in bytes for chunked uploads */
    [[nodiscard]] qint64 maxChunkSize() const;
    void setMaxChunkSize(const qint64 maxChunkSize);

    /** Reads settings from env vars where available.
     *
     * Currently reads _initialChunkSize, _minChunkSize, _maxChunkSize,
     * _targetChunkUploadDuration, _parallelNetworkJobs.
     */
    void fillFromEnvironmentVariables();

    /** Ensure min <= initial <= max
     *
     * Previously min/max chunk size values didn't exist, so users might
     * have setups where the chunk size exceeds the new min/max default
     * values. To cope with this, adjust min/max to always include the
     * initial chunk size value.
     */
    void verifyChunkSizes();

    /** A regular expression to match file names
     * If no pattern is provided the default is an invalid regular expression.
     */
    [[nodiscard]] QRegularExpression fileRegex() const;

    /**
     * A pattern like *.txt, matching only file names
     */
    void setFilePattern(const QString &pattern);

    /**
     * A pattern like /own.*\/.*txt matching the full path
     */
    void setPathPattern(const QString &pattern);

    /** sync had been started via nextcloudcmd command line   */
    [[nodiscard]] bool isCmd() const;
    void setIsCmd(const bool isCmd);

private:
    /**
     * Only sync files that match the expression
     * Invalid pattern by default.
     */
    QRegularExpression _fileRegex = QRegularExpression(QStringLiteral("("));

    qint64 _minChunkSize = chunkV2MinChunkSize;
    qint64 _maxChunkSize = chunkV2MaxChunkSize;

    bool _isCmd = false;
};

}
