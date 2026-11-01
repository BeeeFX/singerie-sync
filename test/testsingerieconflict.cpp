/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "syncenginetestutils.h"

#include "common/utility.h"

#include <QtTest>

using namespace OCC;

namespace
{

void newestKeepsName(FakeFolder &fakeFolder)
{
    auto options = fakeFolder.syncEngine().syncOptions();
    options._newestSaveKeepsName = true;
    fakeFolder.syncEngine().setSyncOptions(options);
}

/// The conflicted copies of `path` in a state.
QStringList copiesOf(FileInfo state, const QString &path)
{
    const auto folder = path.section(QLatin1Char('/'), 0, -2);
    const auto base = QFileInfo(path).completeBaseName();
    QStringList copies;
    auto *dir = folder.isEmpty() ? &state : state.find(folder);
    if (!dir) {
        return copies;
    }
    for (const auto &child : dir->children) {
        if (child.name.startsWith(base) && Utility::isConflictFile(child.name)) {
            copies.append(folder.isEmpty() ? child.name : folder + QLatin1Char('/') + child.name);
        }
    }
    return copies;
}

} // namespace

class TestSingerieConflict : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /*
     * Baptiste (2 November 2026): in a conflict, the newest save keeps the scene's name; the other becomes the
     * conflicted copy. Here this PC's save is the newest: it keeps the name and goes up; the server's older file is
     * kept as the copy, on the PC and then on the server.
     */
    void testLocalNewestKeepsTheName()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        newestKeepsName(fakeFolder);
        QSignalSpy resolved(&fakeFolder.syncEngine(), &SyncEngine::conflictResolved);
        const auto now = QDateTime::currentDateTimeUtc();

        fakeFolder.remoteModifier().setContents(QStringLiteral("A/a1"), 'S');
        fakeFolder.remoteModifier().setModTime(QStringLiteral("A/a1"), now.addSecs(-3600));
        fakeFolder.localModifier().setContents(QStringLiteral("A/a1"), 'L');
        fakeFolder.localModifier().setModTime(QStringLiteral("A/a1"), now);
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(resolved.count(), 1);
        QCOMPARE(resolved.first().at(0).toString(), QStringLiteral("A/a1"));
        QVERIFY(resolved.first().at(2).toBool()); // kept from this PC
        const auto copy = resolved.first().at(1).toString();
        QVERIFY(Utility::isConflictFile(copy));
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("A/a1"))->contentChar, 'L');
        QCOMPARE(fakeFolder.currentLocalState().find(copy)->contentChar, 'S');

        // The next sync sends the newest up at the name. The copy stays on this PC, as upstream's copies do when the
        // server doesn't ask for them (uploadConflictFiles); the server keeps the older one in its versions.
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/a1"))->contentChar, 'L');
        QVERIFY(copiesOf(fakeFolder.currentRemoteState(), QStringLiteral("A/a1")).isEmpty());
        QCOMPARE(copiesOf(fakeFolder.currentLocalState(), QStringLiteral("A/a1")), QStringList{copy});
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("A/a1"))->contentChar, 'L');
    }

    /*
     * The server's save is the newest: it keeps the name, this PC's older file is the copy (upstream's way).
     */
    void testServerNewestKeepsTheName()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        newestKeepsName(fakeFolder);
        QSignalSpy resolved(&fakeFolder.syncEngine(), &SyncEngine::conflictResolved);
        const auto now = QDateTime::currentDateTimeUtc();

        fakeFolder.remoteModifier().setContents(QStringLiteral("A/a1"), 'S');
        fakeFolder.remoteModifier().setModTime(QStringLiteral("A/a1"), now);
        fakeFolder.localModifier().setContents(QStringLiteral("A/a1"), 'L');
        fakeFolder.localModifier().setModTime(QStringLiteral("A/a1"), now.addSecs(-3600));
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(resolved.count(), 1);
        QVERIFY(!resolved.first().at(2).toBool()); // kept from the server
        const auto copy = resolved.first().at(1).toString();
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("A/a1"))->contentChar, 'S');
        QCOMPARE(fakeFolder.currentLocalState().find(copy)->contentChar, 'L');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/a1"))->contentChar, 'S');
    }

    /*
     * SYNC-3: the first sync of an adopted (or moved, or taken-back) folder has no journal row: a file that differs
     * from the server's, however new, becomes the copy; the server's keeps the name.
     */
    void testFirstSyncOfAnAdoptedFolderKeepsTheServersFile()
    {
        FakeFolder fakeFolder{FileInfo{}};
        newestKeepsName(fakeFolder);
        QSignalSpy resolved(&fakeFolder.syncEngine(), &SyncEngine::conflictResolved);
        const auto now = QDateTime::currentDateTimeUtc();
        fakeFolder.remoteModifier().insert(QStringLiteral("scene.blend"), 16, 'S');
        fakeFolder.remoteModifier().setModTime(QStringLiteral("scene.blend"), now.addSecs(-3600));
        fakeFolder.localModifier().insert(QStringLiteral("scene.blend"), 20, 'L');
        fakeFolder.localModifier().setModTime(QStringLiteral("scene.blend"), now);
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(resolved.count(), 1);
        QVERIFY(!resolved.first().at(2).toBool()); // kept from the server
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("scene.blend"))->contentChar, 'S');
        QCOMPARE(fakeFolder.currentLocalState().find(resolved.first().at(1).toString())->contentChar, 'L');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("scene.blend"))->contentChar, 'S');
    }

    /*
     * SYNC-13: a local date more than ten minutes in the future (a PC whose clock is ahead) doesn't win.
     */
    void testAFutureDateDoesNotWin()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        newestKeepsName(fakeFolder);
        QSignalSpy resolved(&fakeFolder.syncEngine(), &SyncEngine::conflictResolved);
        const auto now = QDateTime::currentDateTimeUtc();
        fakeFolder.remoteModifier().setContents(QStringLiteral("A/a1"), 'S');
        fakeFolder.remoteModifier().setModTime(QStringLiteral("A/a1"), now.addSecs(-60));
        fakeFolder.localModifier().setContents(QStringLiteral("A/a1"), 'L');
        fakeFolder.localModifier().setModTime(QStringLiteral("A/a1"), now.addDays(1));
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(resolved.count(), 1);
        QVERIFY(!resolved.first().at(2).toBool());
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("A/a1"))->contentChar, 'S');
    }

    /*
     * A file the member may not write on the server keeps the server's version at the name, however new this PC's.
     */
    void testReadOnlyServerFileKeepsTheName()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        newestKeepsName(fakeFolder);
        QSignalSpy resolved(&fakeFolder.syncEngine(), &SyncEngine::conflictResolved);
        const auto now = QDateTime::currentDateTimeUtc();
        fakeFolder.remoteModifier().find(QStringLiteral("A/a1"))->permissions = RemotePermissions::fromServerString(QStringLiteral("G"));
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().setContents(QStringLiteral("A/a1"), 'S');
        fakeFolder.remoteModifier().setModTime(QStringLiteral("A/a1"), now.addSecs(-3600));
        fakeFolder.localModifier().setContents(QStringLiteral("A/a1"), 'L');
        fakeFolder.localModifier().setModTime(QStringLiteral("A/a1"), now);
        fakeFolder.syncOnce();
        QCOMPARE(fakeFolder.currentLocalState().find(QStringLiteral("A/a1"))->contentChar, 'S');
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("A/a1"))->contentChar, 'S');
    }

    /*
     * A8: a save that writes a new file and renames it over the scene, or moves the scene to a backup first, keeps
     * the scene's identity on the server (its file id, so its history) when both steps are seen in one sync: an
     * update of the scene, never a move of the backup. (Upstream's own move detection, checked here.)
     */
    void testSaveByRenameKeepsTheServerIdentity()
    {
        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.remoteModifier().mkdir(QStringLiteral("work"));
        fakeFolder.remoteModifier().insert(QStringLiteral("work/scene.blend"), 100);
        QVERIFY(fakeFolder.syncOnce());
        const auto fileId = fakeFolder.remoteModifier().find(QStringLiteral("work/scene.blend"))->fileId;

        // Blender: the scene becomes scene.blend1, a new scene.blend is written.
        fakeFolder.localModifier().rename(QStringLiteral("work/scene.blend"), QStringLiteral("work/scene.blend1"));
        fakeFolder.localModifier().insert(QStringLiteral("work/scene.blend"), 120, 'N');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("work/scene.blend"))->fileId, fileId);
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("work/scene.blend"))->size, 120);

        // Natron, Krita: a temporary file renamed over the scene.
        fakeFolder.localModifier().insert(QStringLiteral("work/scene.blend.tmp"), 130, 'T');
        fakeFolder.localModifier().remove(QStringLiteral("work/scene.blend"));
        fakeFolder.localModifier().rename(QStringLiteral("work/scene.blend.tmp"), QStringLiteral("work/scene.blend"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("work/scene.blend"))->fileId, fileId);
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("work/scene.blend"))->size, 130);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }
};

QTEST_GUILESS_MAIN(TestSingerieConflict)
#include "testsingerieconflict.moc"
