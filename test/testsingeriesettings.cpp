/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "account.h"
#include "common/syncjournaldb.h"
#include "owncloudpropagator.h"
#include "singeriesettings.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;

class TestSingerieSettings : public QObject
{
    Q_OBJECT

    static void write(const QString &path, const QByteArray &text)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
    }

private Q_SLOTS:
    void testReadLiveFromTheFile()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);

        // No file: nothing set, the client's own settings apply.
        QCOMPARE(SingerieSettings::uploadLimitKBps(), -1);
        QCOMPARE(SingerieSettings::downloadLimitKBps(), -1);
        QCOMPARE(SingerieSettings::keepFreeBytes(), -1);

        write(path, "[transfers]\nuploadLimitKBps=500\ndownloadLimitKBps=0\n[space]\nkeepFreeBytes=20000000000\n");
        QCOMPARE(SingerieSettings::uploadLimitKBps(), 500);
        QCOMPARE(SingerieSettings::downloadLimitKBps(), 0);
        QCOMPARE(SingerieSettings::keepFreeBytes(), 20000000000LL);

        // Changed while running: read again, no restart.
        write(path, "[transfers]\nuploadLimitKBps=1500\n");
        QCOMPARE(SingerieSettings::uploadLimitKBps(), 1500);
        QCOMPARE(SingerieSettings::downloadLimitKBps(), -1);
        QCOMPARE(SingerieSettings::keepFreeBytes(), -1);

        // Nonsense is ignored, not guessed.
        write(path, "[transfers]\nuploadLimitKBps=fast\ndownloadLimitKBps=-3\n");
        QCOMPARE(SingerieSettings::uploadLimitKBps(), -1);
        QCOMPARE(SingerieSettings::downloadLimitKBps(), -1);

        SingerieSettings::setFilePathForTests({});
    }

    void testPausedReadLive()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);
        QCOMPARE(SingerieSettings::syncPaused(), false);
        write(path, "[sync]\npaused=true\n");
        QCOMPARE(SingerieSettings::syncPaused(), true);
        write(path, "[sync]\npaused=false\n[transfers]\nuploadLimitKBps=10\n");
        QCOMPARE(SingerieSettings::syncPaused(), false);
        SingerieSettings::setFilePathForTests({});
    }

    /*
     * A23: a speed limit (KB/s) is shared by the running transfers; it no longer forces one file at a time. A
     * relative limit (a share of the measured speed) still does.
     */
    void testSpeedLimitKeepsTransfersInParallel()
    {
        QTemporaryDir dir;
        SyncJournalDb journal(dir.filePath(QStringLiteral(".sync_test.db")));
        QSet<QString> blacklist;
        OwncloudPropagator propagator(Account::create(), dir.path(), QStringLiteral("/"), &journal, blacklist);
        SingerieSettings::setFilePathForTests(dir.filePath(QStringLiteral("singerie.ini")));
        SyncOptions options;
        options._parallelNetworkJobs = 6;
        propagator.setSyncOptions(options);
        QCOMPARE(propagator.maximumActiveTransferJob(), 8);
        propagator._uploadLimit = 500 * 1000;
        propagator._downloadLimit = 2000 * 1000;
        QCOMPARE(propagator.maximumActiveTransferJob(), 8);
        propagator._uploadLimit = -50;
        QCOMPARE(propagator.maximumActiveTransferJob(), 1);
        SingerieSettings::setFilePathForTests({});
    }

    /*
     * Round 3: `[transfers] parallel` says how many files move at once (8 by default, upstream's 3 at most), read
     * live; the quick jobs get room beside them.
     */
    void testParallelTransfersReadLive()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);
        SyncJournalDb journal(dir.filePath(QStringLiteral(".sync_test.db")));
        QSet<QString> blacklist;
        OwncloudPropagator propagator(Account::create(), dir.path(), QStringLiteral("/"), &journal, blacklist);
        SyncOptions options;
        options._parallelNetworkJobs = 5; // the server's max_parallel_count
        propagator.setSyncOptions(options);
        QCOMPARE(SingerieSettings::transfersParallel(), 8);
        QCOMPARE(propagator.maximumActiveTransferJob(), 8);
        QCOMPARE(propagator.hardMaximumActiveJob(), 10);
        write(path, "[transfers]\nparallel=16\n");
        QCOMPARE(propagator.maximumActiveTransferJob(), 16);
        QCOMPARE(propagator.hardMaximumActiveJob(), 18);
        write(path, "[transfers]\nparallel=1\n");
        QCOMPARE(propagator.maximumActiveTransferJob(), 1);
        QCOMPARE(propagator.hardMaximumActiveJob(), 5);
        // Out of range or nonsense: the default, not a guess.
        write(path, "[transfers]\nparallel=500\n");
        QCOMPARE(SingerieSettings::transfersParallel(), 8);
        write(path, "[transfers]\nparallel=0\n");
        QCOMPARE(SingerieSettings::transfersParallel(), 8);
        write(path, "[transfers]\nparallel=many\n");
        QCOMPARE(SingerieSettings::transfersParallel(), 8);
        SingerieSettings::setFilePathForTests({});
    }

    /*
     * Round 3: `[hydration] progressive`, the endings of the files an app may start reading while they arrive.
     */
    void testProgressiveEndings()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);
        QVERIFY(SingerieSettings::progressiveSuffixes().isEmpty());
        write(path, "[hydration]\nprogressive=mov, MP4,bgeo.sc,.exr,a/b,\n");
        QCOMPARE(SingerieSettings::progressiveSuffixes(), (QStringList{QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("bgeo.sc")}));
        const auto endings = SingerieSettings::progressiveSuffixes();
        QVERIFY(SingerieSettings::matchesSuffix(QStringLiteral("C:\\P\\preview_v003.MOV"), endings));
        QVERIFY(SingerieSettings::matchesSuffix(QStringLiteral("sim.0001.bgeo.sc"), endings));
        QVERIFY(!SingerieSettings::matchesSuffix(QStringLiteral("shot.0001.exr"), endings));
        QVERIFY(!SingerieSettings::matchesSuffix(QStringLiteral("notamov"), endings));
        SingerieSettings::setFilePathForTests({});
    }

    /*
     * Bulk upload: off unless Singerie turns it on, then only where the server offers it, for small files that
     * haven't changed in the last minute (a file still being written goes up in chunks, A10).
     */
    void testBulkUploadOnlyForSettledSmallFiles()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);
        SyncJournalDb journal(dir.filePath(QStringLiteral(".sync_test.db")));
        QSet<QString> blacklist;
        auto account = Account::create();
        account->setCapabilities({{QStringLiteral("dav"), QVariantMap{{QStringLiteral("bulkupload"), QStringLiteral("1.0")}}}});
        OwncloudPropagator propagator(account, dir.path(), QStringLiteral("/"), &journal, blacklist);
        SyncOptions options;
        options._wholeUploadAge = std::chrono::seconds(60);
        propagator.setSyncOptions(options);

        const auto item = [](qint64 size, qint64 age) {
            auto it = SyncFileItemPtr::create();
            it->_file = QStringLiteral("render/shot.0001.exr");
            it->_size = size;
            it->_modtime = QDateTime::currentSecsSinceEpoch() - age;
            return it;
        };
        const auto smallSize = options.minChunkSize() / 4;
        QCOMPARE(propagator.isDelayedUploadItem(item(smallSize, 600)), false); // off by default
        write(path, "[transfers]\nbulkUpload=true\n");
        QCOMPARE(propagator.isDelayedUploadItem(item(smallSize, 600)), true);
        QCOMPARE(propagator.isDelayedUploadItem(item(smallSize, 5)), false); // changed in the last minute
        QCOMPARE(propagator.isDelayedUploadItem(item(options.minChunkSize(), 600)), false); // not small
        propagator.addToBulkUploadBlackList(QStringLiteral("render/shot.0001.exr"));
        QCOMPARE(propagator.isDelayedUploadItem(item(smallSize, 600)), false); // failed in bulk before
        propagator.removeFromBulkUploadBlackList(QStringLiteral("render/shot.0001.exr"));

        auto plain = Account::create();
        OwncloudPropagator noBulk(plain, dir.path(), QStringLiteral("/"), &journal, blacklist);
        noBulk.setSyncOptions(options);
        QCOMPARE(noBulk.isDelayedUploadItem(item(smallSize, 600)), false); // the server doesn't offer it
        SingerieSettings::setFilePathForTests({});
    }
};

QTEST_GUILESS_MAIN(TestSingerieSettings)
#include "testsingeriesettings.moc"
