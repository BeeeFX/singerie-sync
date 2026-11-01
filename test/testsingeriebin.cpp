/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "singeriebin.h"
#include "singeriesettings.h"
#include "syncenginetestutils.h"

#include "common/syncjournalfilerecord.h"
#include "filesystem.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

using namespace OCC;

namespace
{

/// A bin of its own for each test (FakeFolder's sync folder is a temporary folder: its bin goes beside it), removed
/// afterwards.
struct TestBin {
    explicit TestBin(const QString &syncRoot)
        : bin(QSharedPointer<SingerieBin>::create(syncRoot, QUuid::createUuid().toString(QUuid::Id128).left(12)))
    {
    }
    ~TestBin()
    {
        QDir(bin->folder()).removeRecursively();
        QDir().rmdir(QFileInfo(bin->folder()).absolutePath()); // .singerie-bin, when nothing else is in it
    }
    QSharedPointer<SingerieBin> bin;
};

void useBin(FakeFolder &fakeFolder, const QSharedPointer<SingerieBin> &bin)
{
    auto options = fakeFolder.syncEngine().syncOptions();
    options._bin = bin;
    options._holdWhenEverythingVanishes = true;
    fakeFolder.syncEngine().setSyncOptions(options);
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

} // namespace

class TestSingerieBin : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    /*
     * What the server deletes goes to the bin beside the sync folder, and comes back from it (no download) when the
     * server's file is back the same; a changed file is downloaded; the member's own deletes don't go to the bin.
     */
    void testServerDeletesGoToTheBinAndComeBack()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        TestBin testBin(fakeFolder.localPath());
        const auto &bin = testBin.bin;
        useBin(fakeFolder, bin);
        auto gets = 0;
        fakeFolder.setServerOverride([&gets](QNetworkAccessManager::Operation op, const QNetworkRequest &, QIODevice *) -> QNetworkReply * {
            if (op == QNetworkAccessManager::GetOperation) {
                ++gets;
            }
            return nullptr;
        });
        QCOMPARE(QFileInfo(QFileInfo(bin->folder()).absolutePath()).absolutePath(), QFileInfo(QDir::cleanPath(fakeFolder.localPath())).absolutePath());
        QVERIFY(bin->folder().contains(QStringLiteral("/.singerie-bin/")));

        // The server deletes a file: it lands in the bin, at its path.
        const auto a1 = *fakeFolder.remoteModifier().find(QStringLiteral("A/a1"));
        fakeFolder.remoteModifier().remove(QStringLiteral("A/a1"));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("A/a1")));
        QVERIFY(QFileInfo::exists(bin->folder() + QStringLiteral("/A/a1")));
        QCOMPARE(QFileInfo(bin->folder() + QStringLiteral("/A/a1")).size(), a1.size);
        QCOMPARE(bin->summary().count, 1);
        QCOMPARE(bin->summary().bytes, a1.size);

        // It comes back the same on the server (restored from its trash): moved back, nothing downloaded.
        gets = 0;
        fakeFolder.remoteModifier().insert(QStringLiteral("A/a1"), a1.size, a1.contentChar);
        auto *back = fakeFolder.remoteModifier().find(QStringLiteral("A/a1"));
        back->fileId = a1.fileId;
        back->etag = a1.etag;
        back->lastModified = a1.lastModified;
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(gets, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(bin->summary().count, 0);
        QVERIFY(!QFileInfo::exists(bin->folder() + QStringLiteral("/A/a1")));

        // Deleted again, then back changed: downloaded, the bin's copy stays.
        fakeFolder.remoteModifier().remove(QStringLiteral("A/a1"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(bin->summary().count, 1);
        gets = 0;
        fakeFolder.remoteModifier().insert(QStringLiteral("A/a1"), a1.size + 10, 'X');
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(gets, 1);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(bin->summary().count, 1);

        // The member's own delete goes to the server, not to the bin.
        fakeFolder.localModifier().remove(QStringLiteral("A/a2"));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("A/a2")));
        QCOMPARE(bin->summary().count, 1);

        // A folder deleted on the server is one bin item with its files.
        fakeFolder.remoteModifier().remove(QStringLiteral("B"));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find(QStringLiteral("B")));
        int total = 0;
        const auto items = bin->items(0, 10, &total);
        QCOMPARE(total, 2);
        QCOMPARE(items.first().path, QStringLiteral("B"));
        QVERIFY(items.first().isFolder);
        QCOMPARE(items.first().files, 2);
        QVERIFY(QFileInfo::exists(bin->folder() + QStringLiteral("/B/b1")));

        // Restored whole by hand: back in the sync folder, uploaded since the server no longer has it.
        QString where;
        QCOMPARE(bin->restore(items.first().id, &where, {}), SingerieBin::RestoreResult::Restored);
        QVERIFY(where.endsWith(QStringLiteral("B")));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("B/b1")));
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("B/b2")));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(bin->restore(items.first().id, &where, {}), SingerieBin::RestoreResult::Gone);

        // A restore where something already is, or where the member may not create it, is refused.
        fakeFolder.remoteModifier().remove(QStringLiteral("C/c1"));
        QVERIFY(fakeFolder.syncOnce());
        const auto c1 = bin->items(0, 1, nullptr).first();
        QCOMPARE(c1.path, QStringLiteral("C/c1"));
        QCOMPARE(bin->restore(c1.id,
                              &where,
                              [](const QString &) {
                                  return false;
                              }),
                 SingerieBin::RestoreResult::NotAllowed);
        fakeFolder.localModifier().insert(QStringLiteral("C/c1"));
        QCOMPARE(bin->restore(c1.id, &where, {}), SingerieBin::RestoreResult::Exists);

        // Emptied: nothing left (the files go on the bin's own thread).
        const auto emptied = bin->empty();
        QCOMPARE(emptied.second, 2);
        QCOMPARE(bin->summary().count, 0);
        SingerieBin::waitForDeletes();
        QCOMPARE(QDir(bin->folder()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 1); // the index
    }

    /*
     * A held sync (the server shows the whole folder empty) moves nothing to the bin.
     */
    void testHeldSyncMovesNothing()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        TestBin testBin(fakeFolder.localPath());
        useBin(fakeFolder, testBin.bin);
        const auto keys = fakeFolder.currentRemoteState().children.keys();
        for (const auto &key : keys) {
            fakeFolder.remoteModifier().remove(key);
        }
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("A/a1")));
        QCOMPARE(testBin.bin->summary().count, 0);
    }

    /*
     * Kept retentionDays days; with a size cap, the oldest go first; without one (the default), nothing goes for size.
     */
    void testCleanByAgeAndSize()
    {
        QTemporaryDir dir;
        const auto root = dir.filePath(QStringLiteral("sync"));
        TestBin testBin(root);
        const auto &bin = testBin.bin;
        writeFile(root + QStringLiteral("/old.exr"), QByteArray(100, 'a'));
        writeFile(root + QStringLiteral("/middle.exr"), QByteArray(200, 'b'));
        writeFile(root + QStringLiteral("/new.exr"), QByteArray(300, 'c'));
        QString error;
        for (const auto name : {"old.exr", "middle.exr", "new.exr"}) {
            QVERIFY2(bin->takeFile(QString::fromLatin1(name), SyncJournalFileRecord{}, &error), qPrintable(error));
            QTest::qWait(5); // distinct ids and order
        }
        QCOMPARE(bin->summary().count, 3);
        QCOMPARE(bin->summary().bytes, 600);
        const auto now = QDateTime::currentSecsSinceEpoch();

        // No cap: nothing goes for size.
        QCOMPARE(bin->clean(30, 0, now).second, 0);
        // A cap of 550 bytes: the oldest goes.
        QCOMPARE(bin->clean(30, 550, now).second, 1);
        SingerieBin::waitForDeletes();
        QVERIFY(!QFileInfo::exists(bin->folder() + QStringLiteral("/old.exr")));
        QCOMPARE(bin->summary().bytes, 500);
        // Read again from its index: the same.
        SingerieBin again(root, QFileInfo(bin->folder()).fileName());
        QCOMPARE(again.summary().bytes, 500);
        // 31 days later, everything goes.
        QCOMPARE(bin->clean(30, 0, now + 31 * 24 * 3600).second, 2);
        QCOMPARE(bin->summary().count, 0);
    }

    /*
     * SYNC-10: a big bin stays quick: 20,000 files in the index, each new placeholder's match and each page of the
     * list no longer walk every file (2.5 billion comparisons for 50,000 × 50,000 before).
     */
    void testTwentyThousandFiles()
    {
        QTemporaryDir dir;
        const auto root = dir.filePath(QStringLiteral("sync"));
        TestBin testBin(root);
        QVERIFY(QDir().mkpath(testBin.bin->folder()));
        {
            QFile index(testBin.bin->folder() + QStringLiteral("/.singerie-index.jsonl"));
            QVERIFY(index.open(QIODevice::WriteOnly));
            for (int i = 0; i < 20000; ++i) {
                const auto item = QStringLiteral("item%1").arg(i / 100); // 200 folders of 100 frames
                const QJsonObject file{{QStringLiteral("id"), QStringLiteral("f%1").arg(i)},
                                       {QStringLiteral("item"), item},
                                       {QStringLiteral("path"), QStringLiteral("Projects/A/renders/%1/frame.%2.exr").arg(item).arg(i)},
                                       {QStringLiteral("stored"), QStringLiteral("Projects/A/renders/%1/frame.%2.exr").arg(item).arg(i)},
                                       {QStringLiteral("size"), 1000 + i % 1000},
                                       {QStringLiteral("mtime"), 1700000000 + i},
                                       {QStringLiteral("checksum"), QStringLiteral("SHA1:%1").arg(i)},
                                       {QStringLiteral("etag"), QStringLiteral("e%1").arg(i)},
                                       {QStringLiteral("fileid"), QStringLiteral("id%1").arg(i)},
                                       {QStringLiteral("deleted"), 1700000000 + i / 100}};
                index.write(QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("file"), file}}).toJson(QJsonDocument::Compact)
                            + '\n');
            }
        }
        SingerieBin bin(root, QFileInfo(testBin.bin->folder()).fileName());
        QCOMPARE(bin.summary().count, 200);

        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20000; ++i) {
            // New placeholders the bin doesn't hold: no match, quickly.
            QVERIFY(!bin.findMatch(QStringLiteral("Projects/B/frame.%1.exr").arg(i), 1000 + i % 1000, 1, QByteArrayLiteral("SHA1:x"), "x", "x"));
        }
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("findMatch × 20,000 took %1 ms").arg(timer.elapsed())));

        timer.restart();
        int total = 0;
        for (int page = 0; page < 100; ++page) {
            QCOMPARE(bin.items(page * 2, 2, &total).size(), 2);
        }
        QCOMPARE(total, 200);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("100 pages took %1 ms").arg(timer.elapsed())));
        QCOMPARE(bin.items(0, 1, nullptr).first().path, QStringLiteral("Projects/A/renders/item199"));
    }

    /*
     * SYNC-11: a file moved into the bin whose index line never got written is found by the daily look, kept as an
     * item from then on, and cleaned with the others when its time comes.
     */
    void testOrphansAreFoundAndAged()
    {
        QTemporaryDir dir;
        const auto root = dir.filePath(QStringLiteral("sync"));
        TestBin testBin(root);
        const auto &bin = testBin.bin;
        writeFile(root + QStringLiteral("/kept.exr"), QByteArray(10, 'k'));
        QString error;
        QVERIFY2(bin->takeFile(QStringLiteral("kept.exr"), SyncJournalFileRecord{}, &error), qPrintable(error));
        writeFile(bin->folder() + QStringLiteral("/lost/frame.exr"), QByteArray(20, 'l'));
        QCOMPARE(bin->summary().count, 1);

        const auto now = QDateTime::currentSecsSinceEpoch();
        QCOMPARE(bin->clean(30, 0, now).second, 0);
        QCOMPARE(bin->summary().count, 2);
        QCOMPARE(bin->summary().bytes, 30);
        // Read again from the index: still there.
        SingerieBin again(root, QFileInfo(bin->folder()).fileName());
        QCOMPARE(again.summary().count, 2);
        // Thirty-one days later, both go.
        QCOMPARE(bin->clean(30, 0, now + 31 * 24 * 3600).second, 2);
        SingerieBin::waitForDeletes();
        QVERIFY(!QFileInfo::exists(bin->folder() + QStringLiteral("/lost/frame.exr")));
    }

    /*
     * SYNC-11: the bin keeps the journal's checksum only for a file still as the journal describes it: one changed
     * on the PC since (a save during the sync) can't come back later as "the server's file".
     */
    void testAChangedFileKeepsNoChecksum()
    {
        QTemporaryDir dir;
        const auto root = dir.filePath(QStringLiteral("sync"));
        TestBin testBin(root);
        const auto &bin = testBin.bin;
        writeFile(root + QStringLiteral("/same.exr"), QByteArray(10, 's'));
        writeFile(root + QStringLiteral("/changed.exr"), QByteArray(10, 'c'));
        const auto record = [&root](const QString &name) {
            SyncJournalFileRecord made;
            made._path = name.toUtf8();
            made._type = ItemTypeFile;
            made._fileSize = 10;
            made._modtime = FileSystem::getModTime(root + QLatin1Char('/') + name);
            made._checksumHeader = "SHA1:abc";
            made._etag = "etag";
            made._fileId = "fileid";
            return made;
        };
        auto changed = record(QStringLiteral("changed.exr"));
        changed._modtime -= 100; // the journal saw an earlier save
        QString error;
        QVERIFY2(bin->takeFile(QStringLiteral("same.exr"), record(QStringLiteral("same.exr")), &error), qPrintable(error));
        QVERIFY2(bin->takeFile(QStringLiteral("changed.exr"), changed, &error), qPrintable(error));
        // Matched by the server's checksum: only the unchanged file.
        QVERIFY(bin->findMatch(QStringLiteral("elsewhere/same.exr"), 10, 0, QByteArrayLiteral("SHA1:abc"), {}, {}));
        const auto found = bin->findMatch(QStringLiteral("elsewhere/changed.exr"), 10, 0, QByteArrayLiteral("SHA1:abc"), {}, {});
        QVERIFY(!found || found->path == QStringLiteral("same.exr"));
    }

    /*
     * The settings: 30 days and no cap by default; read live.
     */
    void testSettings()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("singerie.ini"));
        SingerieSettings::setFilePathForTests(path);
        QCOMPARE(SingerieSettings::binRetentionDays(), 30);
        QCOMPARE(SingerieSettings::binMaxBytes(), 0);
        writeFile(path, "[bin]\nretentionDays=7\nmaxBytes=1000000\n");
        QCOMPARE(SingerieSettings::binRetentionDays(), 7);
        QCOMPARE(SingerieSettings::binMaxBytes(), 1000000);
        writeFile(path, "[bin]\nretentionDays=900\n");
        QCOMPARE(SingerieSettings::binRetentionDays(), 30);
        SingerieSettings::setFilePathForTests({});
    }

    /*
     * No bin for a sync folder at a drive's root (no folder beside it to put it in).
     */
    void testNoBinAtADrivesRoot()
    {
#ifdef Q_OS_WIN
        QVERIFY(SingerieBin::folderFor(QStringLiteral("D:/"), QStringLiteral("abc")).isEmpty());
        QCOMPARE(SingerieBin::folderFor(QStringLiteral("D:/Pirouette Prod"), QStringLiteral("abc")), QStringLiteral("D:/.singerie-bin/abc"));
#endif
        QCOMPARE(SingerieBin::rootIdFromJournalPath(QStringLiteral("D:/x/.sync_0123456789ab.db")), QStringLiteral("0123456789ab"));
    }

#ifdef Q_OS_WIN
    /*
     * The bin gets the sync folder's private access rules, protected. The private sync folder gives the farm account
     * read access; this PC has no SingerieFarm account, so a stand-in group (Backup Operators, S-1-5-32-551) plays it.
     */
    void testBinCopiesThePrivateRules()
    {
        QTemporaryDir dir;
        const auto root = dir.filePath(QStringLiteral("sync"));
        QVERIFY(QDir().mkpath(root));
        // The private sync folder as Singerie makes it: the member and SYSTEM Full control, the farm reads; protected.
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        QByteArray userBuffer(512, 0);
        DWORD length = 0;
        QVERIFY(GetTokenInformation(token, TokenUser, userBuffer.data(), userBuffer.size(), &length));
        CloseHandle(token);
        LPWSTR userSid = nullptr;
        QVERIFY(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(userBuffer.data())->User.Sid, &userSid));
        const auto sddl = QStringLiteral("D:P(A;OICI;FA;;;%1)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;S-1-5-32-551)").arg(QString::fromWCharArray(userSid));
        LocalFree(userSid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<const wchar_t *>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr));
        QVERIFY(SetFileSecurityW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(root).utf16()),
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 descriptor));
        LocalFree(descriptor);

        TestBin testBin(root);
        writeFile(root + QStringLiteral("/frame.exr"), "frame");
        QString error;
        QVERIFY2(testBin.bin->takeFile(QStringLiteral("frame.exr"), SyncJournalFileRecord{}, &error), qPrintable(error));

        PSECURITY_DESCRIPTOR binDescriptor = nullptr;
        PACL dacl = nullptr;
        QCOMPARE(GetNamedSecurityInfoW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(testBin.bin->folder()).utf16()),
                                       SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION,
                                       nullptr,
                                       nullptr,
                                       &dacl,
                                       nullptr,
                                       &binDescriptor),
                 static_cast<DWORD>(ERROR_SUCCESS));
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        QVERIFY(GetSecurityDescriptorControl(binDescriptor, &control, &revision));
        QVERIFY(control & SE_DACL_PROTECTED);
        LPWSTR binSddl = nullptr;
        QVERIFY(ConvertSecurityDescriptorToStringSecurityDescriptorW(binDescriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &binSddl, nullptr));
        const auto text = QString::fromWCharArray(binSddl);
        LocalFree(binSddl);
        LocalFree(binDescriptor);
        QVERIFY2(text.contains(QStringLiteral("S-1-5-32-551")) || text.contains(QStringLiteral(";BO)")), qPrintable(text)); // the farm's stand-in reads
        QVERIFY2(text.contains(QStringLiteral(";SY)")), qPrintable(text));
        for (const auto other : {";BU)", ";AU)", ";WD)", ";BA)"}) { // Users, Authenticated Users, Everyone, Administrators
            QVERIFY2(!text.contains(QString::fromLatin1(other)), qPrintable(text));
        }
    }
#endif
};

QTEST_GUILESS_MAIN(TestSingerieBin)
#include "testsingeriebin.moc"
