/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "accountstate.h"
#include "config.h"
#include "configfile.h"
#include "folder.h"
#include "folderman.h"
#include "singeriebin.h"
#include "socketapi/socketapi.h"
#include "socketapi/socketapi_p.h"
#include "syncengine.h"
#include "syncenginetestutils.h"
#include "sslerrordialog.h"
#include "systray.h"
#include "testhelper.h"
#include "theme.h"

#include "common/syncjournalfilerecord.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;

namespace
{

/// A listener that keeps what the socket API sends it.
class RecordingListener : public SocketListener
{
public:
    RecordingListener()
        : SocketListener(nullptr)
    {
    }
    void sendMessage(const QString &message, bool = false) const override
    {
        messages.append(message);
    }

    /// The JSON of the last line starting with `prefix:`.
    [[nodiscard]] QJsonObject last(const QString &prefix) const
    {
        for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
            if (it->startsWith(prefix + QLatin1Char(':'))) {
                return QJsonDocument::fromJson(it->mid(prefix.size() + 1).toUtf8()).object();
            }
        }
        return {};
    }
    [[nodiscard]] QList<QJsonObject> events(const QString &type) const
    {
        QList<QJsonObject> found;
        for (const auto &message : messages) {
            if (message.startsWith(QStringLiteral("SINGERIE_EVENT:"))) {
                const auto event = QJsonDocument::fromJson(message.mid(15).toUtf8()).object();
                if (event.value(QStringLiteral("type")).toString() == type) {
                    found.append(event);
                }
            }
        }
        return found;
    }
    mutable QStringList messages;
};

QJsonObject ask(const QString &command, const QString &argument, RecordingListener &listener)
{
    const auto ok = QMetaObject::invokeMethod(FolderMan::instance()->socketApi(),
                                              ("command_" + command).toLatin1().constData(),
                                              Qt::DirectConnection,
                                              Q_ARG(QString, argument),
                                              Q_ARG(OCC::SocketListener *, &listener));
    Q_ASSERT(ok);
    return listener.last(command);
}

} // namespace

class TestSingerieSocketApi : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    /*
     * Every SINGERIE_* command answers one line NAME:<json>; the holds, the local wipe, conflicts and the bin show in
     * them and in the pushed events.
     */
    void testCommandsAndEvents()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync"))));
        QVERIFY(folder);
        const auto root = QDir::toNativeSeparators(dir.filePath(QStringLiteral("sync")));

        RecordingListener listener;
        const auto hello = ask(QStringLiteral("SINGERIE_HELLO"), {}, listener);
        QCOMPARE(hello.value(QStringLiteral("protocol")).toInt(), 2);
        QVERIFY(hello.value(QStringLiteral("features")).toArray().contains(QStringLiteral("folder_holds")));
        QVERIFY(hello.value(QStringLiteral("version")).toString().contains(QStringLiteral(".sng")));
        QVERIFY(hello.value(QStringLiteral("bin")).toBool());
        QVERIFY(hello.value(QStringLiteral("adopt")).toBool()); // another client's folder, online-only files included

        auto status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QVERIFY(status.contains(QStringLiteral("state")));
        QCOMPARE(status.value(QStringLiteral("held")).toArray().size(), 0);
        QCOMPARE(status.value(QStringLiteral("local_wipes")).toArray().size(), 0);
        QVERIFY(status.value(QStringLiteral("errors")).isArray());

        RecordingListener subscriber;
        QVERIFY(ask(QStringLiteral("SINGERIE_SUBSCRIBE"), {}, subscriber).value(QStringLiteral("ok")).toBool());

        // The server shows the folder empty: held, in the status and as an event, until released.
        QVERIFY(!ask(QStringLiteral("SINGERIE_RELEASE_HOLD"), root, listener).value(QStringLiteral("ok")).toBool());
        Q_EMIT folder->syncEngine().syncHeld(SyncFileItem::Down);
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("held"));
        QCOMPARE(status.value(QStringLiteral("held")).toArray().first().toObject().value(QStringLiteral("folder")).toString(), root);
        QCOMPARE(subscriber.events(QStringLiteral("held")).size(), 1);
        QCOMPARE(subscriber.events(QStringLiteral("held")).first().value(QStringLiteral("direction")).toString(), QStringLiteral("server"));
        QVERIFY(!subscriber.events(QStringLiteral("status")).isEmpty());
        const auto releasedAnswer = ask(QStringLiteral("SINGERIE_RELEASE_HOLD"), root, listener);
        QVERIFY(releasedAnswer.value(QStringLiteral("ok")).toBool());
        QVERIFY(releasedAnswer.value(QStringLiteral("expires")).toInteger() > QDateTime::currentSecsSinceEpoch());

        // One project held on its own (SYNC-1): its own path in the status, released by that path.
        const auto project = QDir::toNativeSeparators(dir.filePath(QStringLiteral("sync/Projects/FILM01")));
        Q_EMIT folder->syncEngine().folderHeld(QStringLiteral("Projects/FILM01"));
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        auto heldFolders = QStringList{};
        for (const auto &one : status.value(QStringLiteral("held")).toArray()) {
            heldFolders.append(one.toObject().value(QStringLiteral("folder")).toString());
        }
        QVERIFY(heldFolders.contains(project));
        QCOMPARE(subscriber.events(QStringLiteral("held")).last().value(QStringLiteral("folder")).toString(), project);
        QVERIFY(!ask(QStringLiteral("SINGERIE_RELEASE_HOLD"), project + QStringLiteral("\\other"), listener).value(QStringLiteral("ok")).toBool());
        QVERIFY(ask(QStringLiteral("SINGERIE_RELEASE_HOLD"), project, listener).value(QStringLiteral("ok")).toBool());
        Q_EMIT folder->syncEngine().folderReleased(QStringLiteral("Projects/FILM01"));
        QCOMPARE(subscriber.events(QStringLiteral("unheld")).last().value(QStringLiteral("folder")).toString(), project);

        // A held local wipe: in the status, an event, answered.
        Q_EMIT folder->syncEngine().localWipeHeld(12);
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        const auto wipe = status.value(QStringLiteral("local_wipes")).toArray().first().toObject();
        QCOMPARE(wipe.value(QStringLiteral("count")).toInt(), 12);
        QCOMPARE(wipe.value(QStringLiteral("folder")).toString(), root);
        QCOMPARE(subscriber.events(QStringLiteral("local_wipe_held")).size(), 1);
        const auto restored = ask(QStringLiteral("SINGERIE_LOCAL_WIPE_RESTORE"), root, listener);
        QVERIFY(restored.value(QStringLiteral("ok")).toBool());
        QCOMPARE(restored.value(QStringLiteral("count")).toInt(), 12);
        QVERIFY(!ask(QStringLiteral("SINGERIE_LOCAL_WIPE_CONFIRM"), QStringLiteral("C:/not/a/sync/folder"), listener).value(QStringLiteral("ok")).toBool());

        // A conflict: pushed as an event when it happens.
        const auto copyName = QStringLiteral("work/scene (conflicted copy 2026-11-02 101500).blend");
        Q_EMIT folder->syncEngine().conflictResolved(QStringLiteral("work/scene.blend"), copyName, true, 2000, 1000);
        const auto conflict = subscriber.events(QStringLiteral("conflict")).value(0);
        QCOMPARE(conflict.value(QStringLiteral("kept_from")).toString(), QStringLiteral("local"));
        QVERIFY(conflict.value(QStringLiteral("kept")).toString().endsWith(QStringLiteral("scene.blend")));

        // The bin: summary with its path beside the sync folder, list, restore, empty.
        const auto bin = folder->localBin();
        QVERIFY(bin && bin->isUsable());
        auto summary = ask(QStringLiteral("SINGERIE_BIN_SUMMARY"), {}, listener);
        QCOMPARE(summary.value(QStringLiteral("count")).toInt(), 0);
        QCOMPARE(summary.value(QStringLiteral("path")).toString(), QDir::toNativeSeparators(bin->folder()));
        QVERIFY(summary.value(QStringLiteral("path")).toString().contains(QStringLiteral(".singerie-bin")));
        QDir().mkpath(dir.filePath(QStringLiteral("sync/work")));
        {
            QFile frame(dir.filePath(QStringLiteral("sync/work/frame.exr")));
            QVERIFY(frame.open(QIODevice::WriteOnly));
            frame.write(QByteArray(1234, 'f'));
        }
        QString error;
        QVERIFY2(bin->takeFile(QStringLiteral("work/frame.exr"), SyncJournalFileRecord{}, &error), qPrintable(error));
        summary = ask(QStringLiteral("SINGERIE_BIN_SUMMARY"), {}, listener);
        QCOMPARE(summary.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(summary.value(QStringLiteral("bytes")).toInteger(), 1234);
        const auto list = ask(QStringLiteral("SINGERIE_BIN_LIST"), QStringLiteral("0\u001e10"), listener);
        QCOMPARE(list.value(QStringLiteral("total")).toInt(), 1);
        const auto item = list.value(QStringLiteral("items")).toArray().first().toObject();
        QCOMPARE(item.value(QStringLiteral("path")).toString(), QStringLiteral("work/frame.exr"));
        QCOMPARE(item.value(QStringLiteral("is_folder")).toBool(), false);
        const auto restore = ask(QStringLiteral("SINGERIE_BIN_RESTORE"), item.value(QStringLiteral("id")).toString(), listener);
        QVERIFY(restore.value(QStringLiteral("ok")).toBool());
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("sync/work/frame.exr"))));
        QCOMPARE(ask(QStringLiteral("SINGERIE_BIN_RESTORE"), item.value(QStringLiteral("id")).toString(), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("gone"));
        QVERIFY(!subscriber.events(QStringLiteral("bin_changed")).isEmpty());
        QVERIFY2(bin->takeFile(QStringLiteral("work/frame.exr"), SyncJournalFileRecord{}, &error), qPrintable(error));
        const auto emptied = ask(QStringLiteral("SINGERIE_BIN_EMPTY"), {}, listener);
        QVERIFY(emptied.value(QStringLiteral("ok")).toBool());
        QCOMPARE(emptied.value(QStringLiteral("count")).toInt(), 1);

        // The nudge.
        QVERIFY(ask(QStringLiteral("SINGERIE_SCAN"), root + QStringLiteral("\\work"), listener).value(QStringLiteral("ok")).toBool());
        QCOMPARE(ask(QStringLiteral("SINGERIE_SCAN"), QStringLiteral("C:/elsewhere"), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("not_in_sync_folder"));

        // Every answer is one line with its command's name.
        for (const auto &message : std::as_const(listener.messages)) {
            QVERIFY2(message.startsWith(QStringLiteral("SINGERIE_")) && !message.contains(QLatin1Char('\n')), qPrintable(message));
        }

        QDir(bin->folder()).removeRecursively();
        QDir().rmdir(QFileInfo(bin->folder()).absolutePath());
        FolderMan::instance()->unloadAndDeleteAllFolders();
    }

    /*
     * A30: a name this PC can't hold (reserved, a trailing space or dot, a forbidden character) or the server refuses
     * is told once as a `skipped_name` event with why, listed in SINGERIE_STATUS's `skipped` after the sync; a
     * trailing space the client renamed says the new name.
     */
    void testSkippedNames()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync/work")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync"))));
        QVERIFY(folder);
        const auto root = dir.filePath(QStringLiteral("sync"));

        const auto item = [](const QString &file, SyncFileItem::Status status) {
            auto made = SyncFileItemPtr::create();
            made->_file = file;
            made->_instruction = CSYNC_INSTRUCTION_IGNORE;
            made->_status = status;
            made->_errorString = QStringLiteral("File name is a reserved name on this file system.");
            return made;
        };
        const auto why = [&root, &item](const QString &file, SyncFileItem::Status status = SyncFileItem::FileNameInvalid) {
            return Folder::singerieSkippedName(root, *item(file, status)).value(QStringLiteral("why")).toString();
        };
        QCOMPARE(why(QStringLiteral("work/aux.blend")), QStringLiteral("reserved"));
        QCOMPARE(why(QStringLiteral("work/CON")), QStringLiteral("reserved"));
        QCOMPARE(why(QStringLiteral("work/com1.txt")), QStringLiteral("reserved"));
        QCOMPARE(why(QStringLiteral("work/notes ")), QStringLiteral("trailing_space"));
        QCOMPARE(why(QStringLiteral("work/ notes")), QStringLiteral("leading_space"));
        QCOMPARE(why(QStringLiteral("work/final.")), QStringLiteral("trailing_dot"));
        QCOMPARE(why(QStringLiteral("work/a:b.txt")), QStringLiteral("invalid_char"));
        QCOMPARE(why(QStringLiteral("work/fine.txt"), SyncFileItem::FileNameInvalidOnServer), QStringLiteral("server"));
        QVERIFY(Folder::singerieSkippedName(root, *item(QStringLiteral("work/fine.txt"), SyncFileItem::Success)).isEmpty());

        // A trailing space renamed on this PC: the new name is told.
        {
            QFile renamed(root + QStringLiteral("/work/notes"));
            QVERIFY(renamed.open(QIODevice::WriteOnly));
        }
        const auto told = Folder::singerieSkippedName(root, *item(QStringLiteral("work/notes "), SyncFileItem::FileNameInvalid));
        QVERIFY(told.value(QStringLiteral("renamed_to")).toString().endsWith(QStringLiteral("notes")));

        RecordingListener listener;
        ask(QStringLiteral("SINGERIE_HELLO"), {}, listener);
        RecordingListener subscriber;
        ask(QStringLiteral("SINGERIE_SUBSCRIBE"), {}, subscriber);
        Q_EMIT folder->syncEngine().itemCompleted(item(QStringLiteral("work/aux.blend"), SyncFileItem::FileNameInvalid), ErrorCategory::GenericError);
        Q_EMIT folder->syncEngine().itemCompleted(item(QStringLiteral("work/aux.blend"), SyncFileItem::FileNameInvalid), ErrorCategory::GenericError);
        const auto events = subscriber.events(QStringLiteral("skipped_name"));
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().value(QStringLiteral("why")).toString(), QStringLiteral("reserved"));
        QVERIFY(events.first().value(QStringLiteral("path")).toString().endsWith(QStringLiteral("aux.blend")));
        QVERIFY(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("skipped")).isArray());
    }

    /*
     * SYNC-6: a conflict is listed from the journal's record while its copy is on this PC: in every SINGERIE_STATUS,
     * after a restart of the socket API too, with which copy kept the name; gone once the copy is deleted.
     */
    void testConflictsOutliveARestart()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync/work")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync"))));
        QVERIFY(folder);

        const auto copyName = QStringLiteral("work/scene (conflicted copy 2026-11-02 101500).blend");
        for (const auto name : {QStringLiteral("work/scene.blend"), copyName}) {
            QFile file(dir.filePath(QStringLiteral("sync/") + name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        ConflictRecord record;
        record.path = copyName.toUtf8();
        record.initialBasePath = "work/scene.blend";
        folder->journalDb()->setConflictRecord(record);
        folder->journalDb()->keyValueStoreSet(SyncEngine::singerieConflictKey("local", copyName), 1);
        folder->journalDb()->keyValueStoreSet(SyncEngine::singerieConflictKey("kept", copyName), 2000);
        folder->journalDb()->keyValueStoreSet(SyncEngine::singerieConflictKey("copy", copyName), 1000);

        RecordingListener listener;
        for (int i = 0; i < 2; ++i) {
            const auto conflicts = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("conflicts")).toArray();
            QCOMPARE(conflicts.size(), 1);
            const auto one = conflicts.first().toObject();
            QCOMPARE(one.value(QStringLiteral("kept_from")).toString(), QStringLiteral("local"));
            QCOMPARE(one.value(QStringLiteral("kept_mtime")).toInteger(), 2000);
            QVERIFY(one.value(QStringLiteral("kept")).toString().endsWith(QStringLiteral("scene.blend")));
            QVERIFY(one.value(QStringLiteral("copy")).toString().contains(QStringLiteral("conflicted copy")));
        }
        // A second socket API (what a restart of the client starts with: nothing in memory): still listed.
        {
            SocketApi restarted;
            RecordingListener fresh;
            QVERIFY(QMetaObject::invokeMethod(&restarted,
                                              "command_SINGERIE_STATUS",
                                              Qt::DirectConnection,
                                              Q_ARG(QString, QString()),
                                              Q_ARG(OCC::SocketListener *, &fresh)));
            QCOMPARE(fresh.last(QStringLiteral("SINGERIE_STATUS")).value(QStringLiteral("conflicts")).toArray().size(), 1);
        }

        // The member deletes the copy: no longer listed.
        QVERIFY(QFile::remove(dir.filePath(QStringLiteral("sync/") + copyName)));
        QCOMPARE(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("conflicts")).toArray().size(), 0);
        FolderMan::instance()->unloadAndDeleteAllFolders();
    }

    /*
     * SYNC-8: the invisible client never shows the "untrusted certificate" window: the certificate is refused and the
     * status says `certificate`.
     */
    void testInvisibleShowsNoTopLevelWindow()
    {
        QVERIFY(Theme::singerieInvisible());
        auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://files.example")));
        SslDialogErrorHandler handler;
        QList<QSslCertificate> certs;
        const QList<QSslError> errors{QSslError(QSslError::SelfSignedCertificate)};
        QVERIFY(!handler.handleErrors(errors, {}, &certs, account));
        QVERIFY(certs.isEmpty());
        QVERIFY(account->property("singerieCertificateRefused").toBool());
        const auto widgets = QApplication::topLevelWidgets();
        for (const auto widget : widgets) {
            QVERIFY2(!widget->isVisible(), qPrintable(widget->windowTitle()));
        }
    }

    /*
     * SYNC-2: an answer applies to the hold or the wipe it was given about, for ten minutes.
     */
    void testAnswersExpire()
    {
        QVERIFY(Folder::singerieAnswerStillGood(1000, 500, 500, 1000));
        QVERIFY(Folder::singerieAnswerStillGood(1000, 500, 500, 1000 + 599));
        QVERIFY(!Folder::singerieAnswerStillGood(1000, 500, 500, 1000 + 600)); // eleven minutes later: dropped
        QVERIFY(!Folder::singerieAnswerStillGood(1000, 500, 700, 1100)); // another hold or wipe since
        QVERIFY(!Folder::singerieAnswerStillGood(1000, 500, 0, 1100)); // nothing held any more
    }

    /*
     * SYNC-5: the status says what the account is doing, and "not set up" with no sync folder.
     */
    void testStatusSaysSignedOut()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        RecordingListener listener;
        auto status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("not_set_up"));
        QCOMPARE(status.value(QStringLiteral("account")).toString(), QStringLiteral("none"));
        QCOMPARE(status.value(QStringLiteral("folders")).toInt(), 0);

        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        QVERIFY(FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync")))));
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QCOMPARE(status.value(QStringLiteral("folders")).toInt(), 1);
        QCOMPARE(status.value(QStringLiteral("account")).toString(), QStringLiteral("ok"));

        accountState->setStateForTesting(AccountState::AskingCredentials);
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QCOMPARE(status.value(QStringLiteral("account")).toString(), QStringLiteral("asking_credentials"));
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("signed_out"));

        accountState->setStateForTesting(AccountState::NetworkError);
        account->setProperty("singerieCertificateRefused", true);
        status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener);
        QCOMPARE(status.value(QStringLiteral("account")).toString(), QStringLiteral("certificate"));
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("certificate"));
        FolderMan::instance()->unloadAndDeleteAllFolders();
    }

    /*
     * Progress within each file (A24): pushed as `progress` events at most once a second while files move (and when a
     * file starts or ends), listed in SINGERIE_STATUS, and one last empty event when nothing moves any more.
     */
    void testProgress()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync"))));
        QVERIFY(folder);

        RecordingListener listener;
        ask(QStringLiteral("SINGERIE_HELLO"), {}, listener);
        RecordingListener subscriber;
        ask(QStringLiteral("SINGERIE_SUBSCRIBE"), {}, subscriber);
        QVERIFY(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("progress")).isNull());

        SyncFileItem big;
        big._file = QStringLiteral("Projects/FILM01/render.exr");
        big._size = 20LL * 1024 * 1024 * 1024;
        big._direction = SyncFileItem::Up;
        big._instruction = CSYNC_INSTRUCTION_NEW;
        big._type = ItemTypeFile;
        SyncFileItem notes = big;
        notes._file = QStringLiteral("Projects/FILM01/notes.txt");
        notes._size = 100;
        notes._direction = SyncFileItem::Down;
        ProgressInfo pi;
        pi.reset();
        pi.adjustTotalsForFile(big);
        pi.adjustTotalsForFile(notes);
        pi._status = ProgressInfo::Propagation;
        pi.setProgressItem(big, 5LL * 1024 * 1024 * 1024);
        pi.setProgressItem(notes, 40);

        const auto body = Folder::singerieProgressOf(pi, dir.filePath(QStringLiteral("sync")));
        const auto files = body.value(QStringLiteral("files")).toArray();
        QCOMPARE(files.size(), 2);
        const auto first = files.first().toObject();
        QVERIFY(first.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("render.exr")));
        QCOMPARE(first.value(QStringLiteral("direction")).toString(), QStringLiteral("up"));
        QCOMPARE(first.value(QStringLiteral("bytes")).toInteger(), 5LL * 1024 * 1024 * 1024);
        QCOMPARE(first.value(QStringLiteral("total")).toInteger(), 20LL * 1024 * 1024 * 1024);
        QCOMPARE(files.at(1).toObject().value(QStringLiteral("direction")).toString(), QStringLiteral("down"));
        QCOMPARE(body.value(QStringLiteral("total")).toInteger(), 20LL * 1024 * 1024 * 1024 + 100);
        QCOMPARE(Folder::singerieProgressOf(pi, dir.path(), 1).value(QStringLiteral("files")).toArray().size(), 1);

        // Pushed once, not again within the second for the same files; a file ending is pushed at once.
        Q_EMIT folder->syncEngine().transmissionProgress(pi);
        QCOMPARE(subscriber.events(QStringLiteral("progress")).size(), 1);
        pi.setProgressItem(big, 6LL * 1024 * 1024 * 1024);
        Q_EMIT folder->syncEngine().transmissionProgress(pi);
        QCOMPARE(subscriber.events(QStringLiteral("progress")).size(), 1);
        const auto status = ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("progress")).toObject();
        QCOMPARE(status.value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("bytes")).toInteger(), 6LL * 1024 * 1024 * 1024);
        pi.setProgressComplete(notes);
        Q_EMIT folder->syncEngine().transmissionProgress(pi);
        QCOMPARE(subscriber.events(QStringLiteral("progress")).size(), 2);
        QCOMPARE(subscriber.events(QStringLiteral("progress")).last().value(QStringLiteral("files")).toArray().size(), 1);
        // No status event for progress alone.
        const auto statusEvents = subscriber.events(QStringLiteral("status"));
        for (const auto &event : statusEvents) {
            QVERIFY(!event.contains(QStringLiteral("progress")));
        }
    }

    /*
     * Round 3: SINGERIE_PREFETCH (Singerie's pre-download of a taken task's shot). Its fetching needs virtual files
     * (tools/nextcloud-build/speed_check.py runs it for real); here: what it answers, and what it refuses.
     */
    void testPrefetchAnswers()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("sync/shot")));
        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.filePath(QStringLiteral("sync"))));
        QVERIFY(folder);

        RecordingListener listener;
        const auto hello = ask(QStringLiteral("SINGERIE_HELLO"), {}, listener);
        QCOMPARE(hello.value(QStringLiteral("prefetch")).toBool(), true);
        QVERIFY(hello.value(QStringLiteral("events")).toArray().contains(QStringLiteral("prefetch")));
        QVERIFY(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("prefetch")).isArray());

        QCOMPARE(ask(QStringLiteral("SINGERIE_PREFETCH"), QStringLiteral("not json"), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("bad_request"));
        QCOMPARE(ask(QStringLiteral("SINGERIE_PREFETCH"), QStringLiteral("{\"paths\": []}"), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("bad_request"));
        const auto outside =
            QJsonDocument(QJsonObject{{QStringLiteral("paths"), QJsonArray{QStringLiteral("C:/elsewhere/shot")}}}).toJson(QJsonDocument::Compact);
        QCOMPARE(ask(QStringLiteral("SINGERIE_PREFETCH"), QString::fromUtf8(outside), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("not_in_sync_folder"));
        const auto inside =
            QJsonDocument(QJsonObject{{QStringLiteral("paths"), QJsonArray{dir.filePath(QStringLiteral("sync/shot"))}}}).toJson(QJsonDocument::Compact);
        // This folder keeps every file on the PC (no virtual files): nothing to pre-download, said so.
        QCOMPARE(ask(QStringLiteral("SINGERIE_PREFETCH"), QString::fromUtf8(inside), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("no_virtual_files"));
        QCOMPARE(ask(QStringLiteral("SINGERIE_PREFETCH_CANCEL"), QStringLiteral("nothing"), listener).value(QStringLiteral("error")).toString(),
                 QStringLiteral("not_found"));
        FolderMan::instance()->unloadAndDeleteAllFolders();
    }

    /*
     * Invisible: the client's pop-ups go to Singerie as events; --version names the build.
     */
    void testInvisible()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(Theme::singerieInvisible());
        QVERIFY(Theme::instance()->versionSwitchOutput().contains(QStringLiteral("Singerie Sync build ")));
        RecordingListener subscriber;
        ask(QStringLiteral("SINGERIE_SUBSCRIBE"), {}, subscriber);
        Systray::instance()->showMessage(QStringLiteral("Sync error"), QStringLiteral("Something to say"));
        const auto notification = subscriber.events(QStringLiteral("notification")).value(0);
        QCOMPARE(notification.value(QStringLiteral("title")).toString(), QStringLiteral("Sync error"));
        QCOMPARE(notification.value(QStringLiteral("message")).toString(), QStringLiteral("Something to say"));
        QVERIFY(!Systray::instance()->isVisible());
    }

    /*
     * Singerie's Pause ([sync] paused in singerie.ini) is read live: the status says paused, and back.
     */
    void testPauseReadLive()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(FolderMan::instance());
        RecordingListener listener;
        QVERIFY(!FolderMan::instance()->singeriePaused());
        {
            QFile ini(ConfigFile().configPath() + QStringLiteral("singerie.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[sync]\npaused=true\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(FolderMan::instance()->singeriePaused(), 10000);
        QCOMPARE(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
        {
            QFile ini(ConfigFile().configPath() + QStringLiteral("singerie.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ini.write("[sync]\npaused=false\n[transfers]\nuploadLimitKBps=1\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(!FolderMan::instance()->singeriePaused(), 10000);
    }

    /*
     * Through the real pipe: SINGERIE_HELLO answers one line.
     */
    void testThroughThePipe()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(FolderMan::instance()->socketApi()); // listening
        QLocalSocket socket;
        socket.connectToServer(QStringLiteral(R"(\\.\pipe\)") + QStringLiteral(APPLICATION_EXECUTABLE) + QLatin1Char('-')
                               + QString::fromLocal8Bit(qgetenv("USERNAME")));
        QVERIFY(socket.waitForConnected(5000));
        socket.write("SINGERIE_HELLO:\n");
        QString answer;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     while (socket.canReadLine()) {
                                         const auto line = QString::fromUtf8(socket.readLine()).trimmed();
                                         if (line.startsWith(QStringLiteral("SINGERIE_HELLO:"))) {
                                             answer = line;
                                         }
                                     }
                                     return !answer.isEmpty();
                                 }()),
                                 10000);
        QCOMPARE(QJsonDocument::fromJson(answer.mid(15).toUtf8()).object().value(QStringLiteral("protocol")).toInt(), 2);
    }
};

QTEST_MAIN(TestSingerieSocketApi)
#include "testsingeriesocketapi.moc"
