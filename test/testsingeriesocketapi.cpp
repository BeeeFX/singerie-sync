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
#include "testhelper.h"

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
        QCOMPARE(hello.value(QStringLiteral("protocol")).toInt(), 1);
        QVERIFY(hello.value(QStringLiteral("version")).toString().contains(QStringLiteral(".sng")));
        QVERIFY(hello.value(QStringLiteral("bin")).toBool());

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
        QVERIFY(ask(QStringLiteral("SINGERIE_RELEASE_HOLD"), root, listener).value(QStringLiteral("ok")).toBool());

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

        // A conflict: pushed, and listed once in the next status.
        Q_EMIT folder->syncEngine().conflictResolved(QStringLiteral("work/scene.blend"),
                                                     QStringLiteral("work/scene (conflicted copy 2026-11-02 101500).blend"),
                                                     true,
                                                     2000,
                                                     1000);
        const auto conflict = subscriber.events(QStringLiteral("conflict")).value(0);
        QCOMPARE(conflict.value(QStringLiteral("kept_from")).toString(), QStringLiteral("local"));
        QVERIFY(conflict.value(QStringLiteral("kept")).toString().endsWith(QStringLiteral("scene.blend")));
        QCOMPARE(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("conflicts")).toArray().size(), 1);
        QCOMPARE(ask(QStringLiteral("SINGERIE_STATUS"), {}, listener).value(QStringLiteral("conflicts")).toArray().size(), 0);

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
        QCOMPARE(QJsonDocument::fromJson(answer.mid(15).toUtf8()).object().value(QStringLiteral("protocol")).toInt(), 1);
    }
};

QTEST_MAIN(TestSingerieSocketApi)
#include "testsingeriesocketapi.moc"
