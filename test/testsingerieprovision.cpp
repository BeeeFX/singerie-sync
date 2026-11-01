/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "accountmanager.h"
#include "accountstate.h"
#include "config.h"
#include "configfile.h"
#include "folderman.h"
#include "singerieprovision.h"
#include "syncenginetestutils.h"
#include "testhelper.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;

class TestSingerieProvision : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    // Singerie Sync never takes another client's accounts: with no settings of its own it finds no account (the
    // official client's nextcloud.cfg beside it is ignored) and waits for --provision.
    void testNoLegacyImport()
    {
        QCOMPARE(DISABLE_ACCOUNT_MIGRATION, 1);
        QTemporaryDir root;
        const auto ours = root.filePath(QStringLiteral("Singerie Sync"));
        QVERIFY(QDir().mkpath(ours));
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("Nextcloud"))));
        QFile official(root.filePath(QStringLiteral("Nextcloud/nextcloud.cfg")));
        QVERIFY(official.open(QIODevice::WriteOnly));
        official.write(
            "[Accounts]\nversion=13\n0\\url=https://cloud.example\n0\\dav_user=anna\n"
            "0\\FoldersWithPlaceholders\\1\\localPath=D:/Pirouette Prod/\n");
        official.close();
        ConfigFile::setConfDir(ours);
        QCOMPARE(AccountManager::instance()->restore(true), AccountManager::AccountsNotFound);
        QVERIFY(AccountManager::instance()->accounts().isEmpty());
        QVERIFY(ConfigFile::discoveredLegacyConfigPath().isEmpty());
    }

    void testParse()
    {
        SingerieProvision::Request request;
        QString error;
        const QByteArray full =
            "{\"server\": \"https://cloud.example\", \"user\": \"anna\", \"app_password\": \"s3cret-Pass\", "
            "\"local_path\": \"D:\\\\Pirouette Prod\",\"remote_path\": \"/\", \"virtual_files\": true, \"adopt\": true, "
            "\"replace_url_from\": [\"https://old.example\"]}";
        QVERIFY(SingerieProvision::parse(full, &request, &error));
        QCOMPARE(request.server, QUrl(QStringLiteral("https://cloud.example")));
        QCOMPARE(request.user, QStringLiteral("anna"));
        QCOMPARE(request.appPassword, QStringLiteral("s3cret-Pass"));
        QCOMPARE(request.localPath, QStringLiteral("D:\\Pirouette Prod"));
        QVERIFY(request.virtualFiles);
        QVERIFY(request.adopt);
        QCOMPARE(request.replaceUrlFrom, QStringList{QStringLiteral("https://old.example")});

        // Defaults: remote "/", virtual files on, no adopt.
        const QByteArray minimal = "{\"server\": \"https://cloud.example\", \"user\": \"anna\", \"app_password\": \"x\", \"local_path\": \"C:/Sync\"}";
        QVERIFY(SingerieProvision::parse(minimal, &request, &error));
        QCOMPARE(request.remotePath, QStringLiteral("/"));
        QVERIFY(request.virtualFiles);
        QVERIFY(!request.adopt);

        // Refused, and the error never repeats the password.
        const QList<QByteArray> bads{
            "not json",
            "{\"server\": \"ftp://x\", \"user\": \"a\", \"app_password\": \"s3cret-Pass\", \"local_path\": \"C:/S\"}",
            // http only for a server on this PC (SYNC-17).
            "{\"server\": \"http://cloud.example\", \"user\": \"a\", \"app_password\": \"s3cret-Pass\", \"local_path\": \"C:/S\"}",
            "{\"server\": \"https://x\", \"user\": \"\", \"app_password\": \"s3cret-Pass\", \"local_path\": \"C:/S\"}",
            "{\"server\": \"https://x\", \"user\": \"a\", \"app_password\": \"s3cret-Pass\", \"local_path\": \"relative\"}",
        };
        for (const auto &bad : bads) {
            QVERIFY(!SingerieProvision::parse(bad, &request, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!error.contains(QStringLiteral("s3cret")));
        }
        const QByteArray bench = "{\"server\": \"http://127.0.0.1:8080\", \"user\": \"a\", \"app_password\": \"x\", \"local_path\": \"C:/S\"}";
        QVERIFY(SingerieProvision::parse(bench, &request, &error));
    }

    void testFailureLine()
    {
        const auto line = SingerieProvision::failure(QStringLiteral("client_running"), QStringLiteral("Quit it first."));
        QVERIFY(!line.contains('\n'));
        const auto object = QJsonDocument::fromJson(line).object();
        QCOMPARE(object.value(QStringLiteral("ok")).toBool(true), false);
        QCOMPARE(object.value(QStringLiteral("error")).toString(), QStringLiteral("client_running"));
    }

    /*
     * The folder checks: a folder holding files needs adopt; a folder inside one already synced is refused.
     */
    void testCheckFolder()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.filePath(QStringLiteral("config")));
        const auto fresh = dir.filePath(QStringLiteral("new"));
        QCOMPARE(SingerieProvision::checkFolder(fresh, false).first, QString());

        const auto full = dir.filePath(QStringLiteral("full"));
        QVERIFY(QDir().mkpath(full));
        QFile file(full + QStringLiteral("/scene.blend"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("blend");
        file.close();
        QCOMPARE(SingerieProvision::checkFolder(full, false).first, QStringLiteral("folder_not_empty"));
        QCOMPARE(SingerieProvision::checkFolder(full, true).first, QString());

        // A folder another client synced (its journal): refused, unless taken over.
        const auto journalOnly = dir.filePath(QStringLiteral("journal"));
        QVERIFY(QDir().mkpath(journalOnly));
        QFile journal(journalOnly + QStringLiteral("/.sync_0123456789ab.db"));
        QVERIFY(journal.open(QIODevice::WriteOnly));
        journal.close();
        QCOMPARE(SingerieProvision::checkFolder(journalOnly, false).first, QStringLiteral("folder_in_other_sync_root"));
        QCOMPARE(SingerieProvision::checkFolder(journalOnly, true).first, QString());

        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);
        const auto synced = dir.filePath(QStringLiteral("synced"));
        QVERIFY(QDir().mkpath(synced));
        QVERIFY(FolderMan::instance()->addFolder(accountState, folderDefinition(synced)));
        QCOMPARE(SingerieProvision::checkFolder(synced + QStringLiteral("/inside"), false).first, QStringLiteral("folder_in_other_sync_root"));
        FolderMan::instance()->unloadAndDeleteAllFolders();
    }
};

QTEST_MAIN(TestSingerieProvision)
#include "testsingerieprovision.moc"
