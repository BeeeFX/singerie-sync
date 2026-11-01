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
        SyncOptions options;
        options._parallelNetworkJobs = 6;
        propagator.setSyncOptions(options);
        QCOMPARE(propagator.maximumActiveTransferJob(), 3);
        propagator._uploadLimit = 500 * 1000;
        propagator._downloadLimit = 2000 * 1000;
        QCOMPARE(propagator.maximumActiveTransferJob(), 3);
        propagator._uploadLimit = -50;
        QCOMPARE(propagator.maximumActiveTransferJob(), 1);
    }
};

QTEST_GUILESS_MAIN(TestSingerieSettings)
#include "testsingeriesettings.moc"
