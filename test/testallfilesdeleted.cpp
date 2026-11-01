/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2017 ownCloud GmbH
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include <QtTest>
#include "syncenginetestutils.h"
#include <syncengine.h>
#include <configfile.h>

using namespace OCC;


static void changeAllFileId(FileInfo &info) {
    info.fileId = generateFileId();
    if (!info.isDir) {
        return;
    }
    info.etag = generateEtag();
    for (auto &child : info.children) {
        changeAllFileId(child);
    }
}

/*
 * This test ensure that the SyncEngine::aboutToRemoveAllFiles is correctly called and that when
 * we the user choose to remove all files SyncJournalDb::clearFileTable makes works as expected
 */
class TestAllFilesDeleted : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testAllFilesDeletedKeep_data()
    {
        QTest::addColumn<bool>("deleteOnRemote");
        QTest::newRow("local") << false;
        QTest::newRow("remote") << true;

    }

    /*
     * In this test, all files are deleted in the client, or the server, and we simulate
     * that the users press "keep"
     */
    void testAllFilesDeletedKeep()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        ConfigFile config;
        config.setPromptDeleteFiles(true);

        //Just set a blacklist so we can check it is still there. This directory does not exists but
        // that does not matter for our purposes.
        QStringList selectiveSyncBlackList = { "Q/" };
        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
                                                                selectiveSyncBlackList);

        auto initialState = fakeFolder.currentLocalState();
        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
                callback(true);
                fakeFolder.syncEngine().journal()->clearFileTable(); // That's what Folder is doing
            });

        auto &modifier = deleteOnRemote ? fakeFolder.remoteModifier() : static_cast<FileModifier&>(fakeFolder.localModifier());
        const auto childrenKeys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &key : childrenKeys) {
            modifier.remove(key);
        }

        QVERIFY(!fakeFolder.syncOnce()); // Should fail because we cancel the sync
        QCOMPARE(aboutToRemoveAllFilesCalled, 1);

        // Next sync should recover all files
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
        QCOMPARE(fakeFolder.currentRemoteState(), initialState);

        // The selective sync blacklist should be not have been deleted.
        bool ok = true;
        QCOMPARE(fakeFolder.syncEngine().journal()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok),
                 selectiveSyncBlackList);
    }

    void testEverythingVanishingIsHeld_data()
    {
        testAllFilesDeletedKeep_data();
    }

    /*
     * A client that shows no window (SyncOptions::_holdWhenEverythingVanishes): when every file would be deleted
     * at once, the sync is held (nothing deleted, no prompt, syncHeld emitted) and a later sync, once the files are
     * back, finds everything in place.
     */
    void testEverythingVanishingIsHeld()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        ConfigFile().setPromptDeleteFiles(true);
        QSignalSpy prompt(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles);
        QSignalSpy serverHeld(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);
        QSignalSpy localHeld(&fakeFolder.syncEngine(), &SyncEngine::localWipeHeld);
        // The server showing it empty is a held sync; this PC showing it empty is a held local wipe.
        auto &held = deleteOnRemote ? serverHeld : localHeld;

        const auto initialState = fakeFolder.currentLocalState();
        const auto saved = deleteOnRemote ? fakeFolder.currentRemoteState() : fakeFolder.currentLocalState();
        auto &modifier = deleteOnRemote ? fakeFolder.remoteModifier() : static_cast<FileModifier &>(fakeFolder.localModifier());
        const auto childrenKeys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &key : childrenKeys) {
            modifier.remove(key);
        }

        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(prompt.count(), 0);
        QCOMPARE(held.count(), 1);
        if (deleteOnRemote) {
            QCOMPARE(held.first().first().value<SyncFileItem::Direction>(), SyncFileItem::Down);
            QCOMPARE(localHeld.count(), 0);
        } else {
            QCOMPARE(held.first().first().toInt(), 8); // every file of the folder
            QCOMPARE(serverHeld.count(), 0);
        }
        // Nothing deleted on the other side.
        if (deleteOnRemote) {
            QCOMPARE(fakeFolder.currentLocalState(), initialState);
        } else {
            QCOMPARE(fakeFolder.currentRemoteState(), initialState);
        }

        // Still held while the folder stays empty.
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 2);

        // The server's files come back (its storage is mounted again): the next sync finds everything in place.
        if (deleteOnRemote) {
            for (auto it = saved.children.cbegin(); it != saved.children.cend(); ++it) {
                fakeFolder.remoteModifier().children.insert(it.key(), it.value());
            }
            QVERIFY(fakeFolder.syncOnce());
            QCOMPARE(fakeFolder.currentLocalState(), initialState);
            QCOMPARE(fakeFolder.currentRemoteState(), initialState);
        }
        QCOMPARE(prompt.count(), 0);
    }

    /*
     * Baptiste's local wipe (2 November 2026): every one of the member's own files deleted on this PC at once is
     * held; nothing goes to the server until the answer. "Put them back" brings them back from the server, "Delete
     * everywhere" sends the deletes. Deleting a few of one's own files follows at once.
     */
    void testLocalWipeIsHeldUntilAnswered()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy held(&fakeFolder.syncEngine(), &SyncEngine::localWipeHeld);
        QSignalSpy answered(&fakeFolder.syncEngine(), &SyncEngine::localWipeAnswered);

        // A colleague's work the member can't delete, and twelve files of the member's own.
        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("bob"));
        remote.insert(QStringLiteral("bob/scene.blend"), 16);
        remote.insert(QStringLiteral("bob/notes.txt"), 16);
        for (const auto name : {"bob", "bob/scene.blend", "bob/notes.txt"}) {
            remote.find(QString::fromLatin1(name))->permissions = RemotePermissions::fromServerString(QStringLiteral("GWNV"));
        }
        remote.mkdir(QStringLiteral("anna"));
        remote.mkdir(QStringLiteral("anna/shots"));
        for (int i = 0; i < 6; ++i) {
            remote.insert(QStringLiteral("anna/comp_v%1.nk").arg(i), 16);
            remote.insert(QStringLiteral("anna/shots/frame.%1.exr").arg(i), 32);
        }
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        const auto everything = fakeFolder.currentRemoteState();

        // A few of her own files deleted: an ordinary delete, followed.
        fakeFolder.localModifier().remove(QStringLiteral("anna/comp_v0.nk"));
        fakeFolder.localModifier().remove(QStringLiteral("anna/comp_v1.nk"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(held.count(), 0);
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("anna/comp_v0.nk")));
        remote.insert(QStringLiteral("anna/comp_v0.nk"), 16);
        remote.insert(QStringLiteral("anna/comp_v1.nk"), 16);
        QVERIFY(fakeFolder.syncOnce());

        // All of her own files at once (her folder): held, the server untouched, held again at the next look.
        fakeFolder.localModifier().remove(QStringLiteral("anna"));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 1);
        QCOMPARE(held.last().first().toInt(), 12);
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("anna/shots/frame.5.exr")));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 2);

        // "Put them back": nothing deleted on the server, the files come back.
        options._localWipeAnswer = SyncOptions::LocalWipeAnswer::PutBack;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(answered.count(), 1);
        options._localWipeAnswer = SyncOptions::LocalWipeAnswer::None;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().children.size(), everything.children.size());
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("anna/shots/frame.5.exr")));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(held.count(), 2);

        // Again, then "Delete everywhere": the deletes go to the server, the colleague's work stays.
        fakeFolder.localModifier().remove(QStringLiteral("anna"));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 3);
        options._localWipeAnswer = SyncOptions::LocalWipeAnswer::DeleteEverywhere;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(answered.count(), 2);
        QCOMPARE(answered.last().first().toInt(), 12);
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("anna")));
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("bob/scene.blend")));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    /*
     * The server showing the whole folder empty is held, until the managing application says the member really
     * left (SINGERIE_RELEASE_HOLD): then the next sync follows.
     */
    void testReleasedHoldFollowsTheServer()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy held(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);
        const auto childrenKeys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &key : childrenKeys) {
            fakeFolder.remoteModifier().remove(key);
        }
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 1);
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("A/a1")));

        options._releaseHold = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(held.count(), 1);
        QCOMPARE(fakeFolder.currentLocalState().children.size(), 0);
    }

    /*
     * Many deletions that leave something behind follow the server, without a prompt, past the threshold too.
     */
    void testManyDeletionsFollowTheServer()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        ConfigFile config;
        config.setPromptDeleteFiles(true);
        QSignalSpy prompt(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles);
        QSignalSpy held(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);

        // A render's frames, deep in a project (a folder near the top would be held on its own: SYNC-1).
        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("Projects"));
        remote.mkdir(QStringLiteral("Projects/FILM01"));
        remote.mkdir(QStringLiteral("Projects/FILM01/frames"));
        for (int i = 0; i < config.deleteFilesThreshold() + 20; ++i) {
            remote.insert(QStringLiteral("Projects/FILM01/frames/frame.%1.exr").arg(i, 4, 10, QLatin1Char('0')), 16);
        }
        remote.insert(QStringLiteral("Projects/FILM01/scene.blend"), 16);
        QVERIFY(fakeFolder.syncOnce());

        remote.remove(QStringLiteral("Projects/FILM01/frames"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(prompt.count(), 0);
        QCOMPARE(held.count(), 0);
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("Projects/FILM01/frames")));
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("Projects/FILM01/scene.blend")));
    }

    /*
     * SYNC-1 (Baptiste's pick of 2 November 2026, built 12 November): a whole project vanishing from the server
     * beside the others is held on its own: nothing deleted in it on this PC, the other project and the studio folder
     * still sync, the next look holds it again; once the managing application releases it, it is followed.
     */
    void testAProjectVanishingIsHeld()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy folderHeld(&fakeFolder.syncEngine(), &SyncEngine::folderHeld);
        QSignalSpy driveHeld(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);
        QSignalSpy released(&fakeFolder.syncEngine(), &SyncEngine::folderReleased);

        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("Projects"));
        for (const auto project : {"A", "B"}) {
            const auto root = QStringLiteral("Projects/") + QString::fromLatin1(project);
            remote.mkdir(root);
            remote.mkdir(root + QStringLiteral("/shots"));
            for (int i = 0; i < 12; ++i) {
                remote.insert(root + QStringLiteral("/shots/frame.%1.exr").arg(i), 16);
            }
        }
        remote.mkdir(QStringLiteral("Studio"));
        remote.insert(QStringLiteral("Studio/tools.txt"), 16);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Project A's folder vanishes on the server (a group taken off its Team folder); B gets a new file.
        remote.remove(QStringLiteral("Projects/A"));
        remote.insert(QStringLiteral("Projects/B/new.blend"), 16);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(driveHeld.count(), 0);
        QCOMPARE(folderHeld.count(), 1);
        QCOMPARE(folderHeld.first().first().toString(), QStringLiteral("Projects/A"));
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("Projects/A/shots/frame.11.exr")));
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("Projects/B/new.blend")));

        // The next look holds it again, with nothing else changed (the parent's new ETag wasn't recorded).
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(folderHeld.count(), 2);
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("Projects/A/shots/frame.11.exr")));

        // Released (the member really left project A): followed.
        options._releasedFolders = {QStringLiteral("Projects/A")};
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(released.count(), 1);
        QCOMPARE(folderHeld.count(), 2);
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("Projects/A")));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // A small folder near the top going is an ordinary deletion, followed.
        options._releasedFolders.clear();
        fakeFolder.syncEngine().setSyncOptions(options);
        remote.remove(QStringLiteral("Studio"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(folderHeld.count(), 2);
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("Studio")));
    }

    /*
     * SYNC-1: an empty folder the member made at the top of the drive (synced, so unchanged in every later sync) no
     * longer switches the whole-drive hold off: "everything" is counted from the journal's files.
     */
    void testAnEmptyLocalFolderDoesNotSwitchTheHoldOff()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy driveHeld(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);

        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("Projects"));
        remote.mkdir(QStringLiteral("Projects/A"));
        for (int i = 0; i < 12; ++i) {
            remote.insert(QStringLiteral("Projects/A/frame.%1.exr").arg(i), 16);
        }
        remote.mkdir(QStringLiteral("Studio"));
        remote.insert(QStringLiteral("Studio/tools.txt"), 16);
        QVERIFY(fakeFolder.syncOnce());
        fakeFolder.localModifier().mkdir(QStringLiteral("New folder"));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("New folder")));
        const auto before = fakeFolder.currentLocalState();

        // Every file vanishes on the server; the empty folder stays.
        remote.remove(QStringLiteral("Projects"));
        remote.remove(QStringLiteral("Studio"));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(driveHeld.count(), 1);
        QCOMPARE(fakeFolder.currentLocalState(), before);
    }

    /*
     * With virtual files the journal knows nothing inside a folder never opened (an online-only folder): each such
     * folder stands for its unknown content, so the one file the journal knows being deleted on the server is an
     * ordinary deletion, not "everything" (found on the bench, 12 November 2026).
     */
    void testUnopenedFoldersCountInTheHold()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy driveHeld(&fakeFolder.syncEngine(), &SyncEngine::syncHeld);

        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("Projects"));
        remote.mkdir(QStringLiteral("Projects/A"));
        remote.insert(QStringLiteral("Projects/A/frame.exr"), 16);
        remote.mkdir(QStringLiteral("Projects/B"));
        remote.mkdir(QStringLiteral("Projects/C"));
        QVERIFY(fakeFolder.syncOnce());
        // B and C as an online-only client keeps them: folders never opened on this PC.
        for (const auto folder : {"Projects/B", "Projects/C"}) {
            SyncJournalFileRecord record;
            QVERIFY(fakeFolder.syncJournal().getFileRecord(QByteArray(folder), &record) && record.isValid());
            record._type = ItemTypeVirtualDirectory;
            QVERIFY(fakeFolder.syncJournal().setFileRecord(record));
        }

        remote.remove(QStringLiteral("Projects/A/frame.exr"));
        fakeFolder.syncOnce();
        QCOMPARE(driveHeld.count(), 0);
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("Projects/A/frame.exr")));
    }

    /*
     * SYNC-2: "Delete everywhere" answers the wipe it was asked about; a bigger wipe found later is held again as a
     * new one, nothing deleted on the server.
     */
    void testAStaleAnswerIsNotApplied()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto options = fakeFolder.syncEngine().syncOptions();
        options._holdWhenEverythingVanishes = true;
        fakeFolder.syncEngine().setSyncOptions(options);
        QSignalSpy held(&fakeFolder.syncEngine(), &SyncEngine::localWipeHeld);
        QSignalSpy answered(&fakeFolder.syncEngine(), &SyncEngine::localWipeAnswered);

        auto &remote = fakeFolder.remoteModifier();
        remote.mkdir(QStringLiteral("anna"));
        for (int i = 0; i < 24; ++i) {
            remote.insert(QStringLiteral("anna/comp_v%1.nk").arg(i), 16);
        }
        QVERIFY(fakeFolder.syncOnce());

        // The member answered a wipe of 12 files; 24 are gone now.
        fakeFolder.localModifier().remove(QStringLiteral("anna"));
        options._localWipeAnswer = SyncOptions::LocalWipeAnswer::DeleteEverywhere;
        options._localWipeAnsweredCount = 12;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(held.count(), 1);
        QCOMPARE(held.last().first().toInt(), 24);
        QCOMPARE(answered.count(), 0);
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("anna/comp_v23.nk")));

        // Answered for these 24: followed.
        options._localWipeAnsweredCount = 24;
        fakeFolder.syncEngine().setSyncOptions(options);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(answered.count(), 1);
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("anna")));
    }

    void testAllFilesDeletedDelete_data()
    {
        testAllFilesDeletedKeep_data();
    }

    /*
     * This test is like the previous one but we simulate that the user presses "delete"
     */
    void testAllFilesDeletedDelete()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
                callback(false);
            });

        auto &modifier = deleteOnRemote ? fakeFolder.remoteModifier() : static_cast<FileModifier&>(fakeFolder.localModifier());
        const auto childrenKeys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &key : childrenKeys) {
            modifier.remove(key);
        }

        QVERIFY(fakeFolder.syncOnce()); // Should succeed, and all files must then be deleted

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);

        // Try another sync to be sure.

        QVERIFY(fakeFolder.syncOnce()); // Should succeed (doing nothing)
        QCOMPARE(aboutToRemoveAllFilesCalled, 1); // should not have been called.

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);
    }

    void testNotDeleteMetaDataChange() {
        /**
         * This test make sure that we don't popup a file deleted message if all the metadata have
         * been updated (for example when the server is upgraded or something)
         **/

        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        // We never remove all files.
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&] { QVERIFY(false); });
        QVERIFY(fakeFolder.syncOnce());

        const auto childrenKeys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &s : childrenKeys) {
            fakeFolder.syncJournal().avoidRenamesOnNextSync(s); // clears all the fileid and inodes.
        }
        fakeFolder.localModifier().remove("A/a1");
        auto expectedState = fakeFolder.currentLocalState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);

        fakeFolder.remoteModifier().remove("B/b1");
        changeAllFileId(fakeFolder.remoteModifier());
        expectedState = fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);
    }

    void testResetServer()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, SyncFileItem::Down);
                callback(false);
            });

        // Some small changes
        fakeFolder.localModifier().mkdir("Q");
        fakeFolder.localModifier().insert("Q/q1");
        fakeFolder.localModifier().appendByte("B/b1");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);

        // Do some change locally
        fakeFolder.localModifier().appendByte("A/a1");

        // reset the server.
        fakeFolder.remoteModifier() = FileInfo::A12_B12_C12_S12();

        // Now, aboutToRemoveAllFiles with down as a direction
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 1);

    }

    void testDataFingetPrint_data()
    {
        QTest::addColumn<bool>("hasInitialFingerPrint");
        QTest::newRow("initial finger print") << true;
        QTest::newRow("no initial finger print") << false;
    }

    void testDataFingetPrint()
    {
        QFETCH(bool, hasInitialFingerPrint);
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        fakeFolder.remoteModifier().setContents("C/c1", 'N');
        fakeFolder.remoteModifier().setModTime("C/c1", QDateTime::currentDateTimeUtc().addDays(-2));
        fakeFolder.remoteModifier().remove("C/c2");
        if (hasInitialFingerPrint) {
            fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>initial_finger_print</oc:data-fingerprint>";
        } else {
            //Server support finger print, but none is set.
            fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint></oc:data-fingerprint>";
        }

        int fingerprintRequests = 0;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation, const QNetworkRequest &request, QIODevice *stream) -> QNetworkReply * {
            auto verb = request.attribute(QNetworkRequest::CustomVerbAttribute).toString();
            if (verb == "PROPFIND") {
                auto data = stream->readAll();
                if (data.contains("data-fingerprint")) {
                    if (request.url().path().endsWith("dav/files/admin/")) {
                        ++fingerprintRequests;
                    } else {
                        fingerprintRequests = -10000; // fingerprint queried on incorrect path
                    }
                }
            }
            return nullptr;
        });

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fingerprintRequests, 1);
        // First sync, we did not change the finger print, so the file should be downloaded as normal
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find("C/c1")->contentChar, 'N');
        QVERIFY(!fakeFolder.currentRemoteState().find("C/c2"));

        /* Simulate a backup restoration */

        // A/a1 is an old file
        fakeFolder.remoteModifier().setContents("A/a1", 'O');
        fakeFolder.remoteModifier().setModTime("A/a1", QDateTime::currentDateTimeUtc().addDays(-2));
        // B/b1 did not exist at the time of the backup
        fakeFolder.remoteModifier().remove("B/b1");
        // B/b2 was uploaded by another user in the mean time.
        fakeFolder.remoteModifier().setContents("B/b2", 'N');
        fakeFolder.remoteModifier().setModTime("B/b2", QDateTime::currentDateTimeUtc().addDays(2));

        // C/c3 was removed since we made the backup
        fakeFolder.remoteModifier().insert("C/c3_removed");
        // C/c4 was moved to A/a2 since we made the backup
        fakeFolder.remoteModifier().rename("A/a2", "C/old_a2_location");

        // The admin sets the data-fingerprint property
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>new_finger_print</oc:data-fingerprint>";

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fingerprintRequests, 2);
        auto currentState = fakeFolder.currentLocalState();
        // Although the local file is kept as a conflict, the server file is downloaded
        QCOMPARE(currentState.find("A/a1")->contentChar, 'O');
        auto conflict = findConflict(currentState, "A/a1");
        QVERIFY(conflict);
        QCOMPARE(conflict->contentChar, 'W');
        fakeFolder.localModifier().remove(conflict->path());
        // b1 was restored (re-uploaded)
        QVERIFY(currentState.find("B/b1"));

        // b2 has the new content (was not restored), since its mode time goes forward in time
        QCOMPARE(currentState.find("B/b2")->contentChar, 'N');
        conflict = findConflict(currentState, "B/b2");
        QVERIFY(conflict); // Just to be sure, we kept the old file in a conflict
        QCOMPARE(conflict->contentChar, 'W');
        fakeFolder.localModifier().remove(conflict->path());

        // We actually do not remove files that technically should have been removed (we don't want data-loss)
        QVERIFY(currentState.find("C/c3_removed"));
        QVERIFY(currentState.find("C/old_a2_location"));

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testSingleFileRenamed() {
        FakeFolder fakeFolder{FileInfo{}};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction , std::function<void(bool)> ) {
                aboutToRemoveAllFilesCalled++;
                QFAIL("should not be called");
            });

        // add a single file
        fakeFolder.localModifier().insert("hello.txt");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // rename it
        fakeFolder.localModifier().rename("hello.txt", "goodbye.txt");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testSelectiveSyncNoPopup() {
        // Unselecting all folder should not cause the popup to be shown
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction , std::function<void(bool)>) {
                aboutToRemoveAllFilesCalled++;
                QFAIL("should not be called");
            });

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
            QStringList() << "A/" << "B/" << "C/" << "S/");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), FileInfo{}); // all files should be one locally
        QCOMPARE(fakeFolder.currentRemoteState(), FileInfo::A12_B12_C12_S12()); // Server not changed
        QCOMPARE(aboutToRemoveAllFilesCalled, 0); // But we did not show the popup
    }


};

QTEST_GUILESS_MAIN(TestAllFilesDeleted)
#include "testallfilesdeleted.moc"
