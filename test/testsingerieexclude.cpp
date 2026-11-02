/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "syncenginetestutils.h"

#include <syncengine.h>

#include <QtTest>

using namespace OCC;

class TestSingerieExclude : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /*
     * A7 (#8186): files synced, then left out by a new pattern, then gone from this PC, then the pattern removed:
     * nothing is deleted on the server (their journal records were forgotten once they were excluded and gone).
     */
    void testNewlyExcludedForgetTheirRecords()
    {
        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.remoteModifier().mkdir(QStringLiteral("work"));
        for (auto i = 1; i <= 5; ++i) {
            fakeFolder.remoteModifier().insert(QStringLiteral("work/frame.%1.tmp").arg(i), 32);
        }
        fakeFolder.remoteModifier().insert(QStringLiteral("work/scene.blend"), 32);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        SyncJournalFileRecord record;
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArrayLiteral("work/frame.1.tmp"), &record) && record.isValid());

        fakeFolder.syncEngine().excludedFiles().addManualExclude(QStringLiteral("*.tmp"));
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        // Still on this PC: the record stays (a change made meanwhile would go up as an ordinary change).
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArrayLiteral("work/frame.1.tmp"), &record) && record.isValid());

        // Gone from this PC while excluded: the records are forgotten; then the pattern is removed.
        for (auto i = 1; i <= 5; ++i) {
            fakeFolder.localModifier().remove(QStringLiteral("work/frame.%1.tmp").arg(i));
        }
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArrayLiteral("work/frame.1.tmp"), &record));
        QVERIFY(!record.isValid());
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArrayLiteral("work/scene.blend"), &record) && record.isValid());
        fakeFolder.syncEngine().excludedFiles().clearManualExcludes();
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());

        for (auto i = 1; i <= 5; ++i) {
            QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("work/frame.%1.tmp").arg(i)));
        }
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    /*
     * A folder newly excluded forgets the records of everything below it too.
     */
    void testNewlyExcludedFolderForgetsBelow()
    {
        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.remoteModifier().mkdir(QStringLiteral("cache"));
        fakeFolder.remoteModifier().insert(QStringLiteral("cache/a.bin"), 16);
        fakeFolder.remoteModifier().insert(QStringLiteral("cache/b.bin"), 16);
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.syncEngine().excludedFiles().addManualExclude(QStringLiteral("cache/"));
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.localModifier().remove(QStringLiteral("cache"));
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        SyncJournalFileRecord record;
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArrayLiteral("cache/a.bin"), &record));
        QVERIFY(!record.isValid());
        fakeFolder.syncEngine().excludedFiles().clearManualExcludes();
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("cache/a.bin")));
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("cache/a.bin")));
    }
};

QTEST_GUILESS_MAIN(TestSingerieExclude)
#include "testsingerieexclude.moc"
