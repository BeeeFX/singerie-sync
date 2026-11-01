/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "accountfwd.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace OCC
{

class AccountState;

/**
 * `--provision`: Singerie sets the account and its one sync folder up without a window and without the app password
 * on the command line. One JSON object on standard input (one line):
 *
 *   {"server", "user", "app_password", "local_path", "remote_path": "/", "virtual_files": true, "adopt": false,
 *    "replace_url_from": ["https://old.example"]}
 *
 * One JSON line on standard output, then the process ends (exit code 0 only when ok):
 *
 *   {"ok": true, "account": "anna@cloud.example", "folder": "D:\\Pirouette Prod"}
 *   {"ok": false, "error": "<code>", "message": "<English sentence>"}
 *
 * Codes: bad_request, bad_credentials, server_unreachable, folder_not_ntfs, folder_in_other_sync_root,
 * folder_not_empty (only without adopt), account_exists (the account already syncs another folder), client_running.
 * The app password goes to Windows' credential store as the client keeps it; it is never logged or written anywhere
 * else. A second provision of the same account updates its app password (and its address, from replace_url_from).
 */
class SingerieProvision : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QUrl server;
        QString user;
        QString appPassword;
        QString localPath;
        QString remotePath = QStringLiteral("/");
        bool virtualFiles = true;
        bool adopt = false;
        QStringList replaceUrlFrom;
    };

    /// Parses the JSON line; on failure `error` says what's wrong (no secret in it).
    static bool parse(const QByteArray &line, Request *request, QString *error);
    /// The answer line for a failure (also used before the job exists: client_running, bad_request).
    static QByteArray failure(const QString &code, const QString &message);

    explicit SingerieProvision(const Request &request, QObject *parent = nullptr);

    /// Starts; ends the application with the answer printed.
    void start();

    /// Folder checks that don't need the server, in order; empty when fine. Public for tests.
    static QPair<QString, QString> checkFolder(const QString &localPath, bool adopt);

private:
    void finish(const QByteArray &answer, bool ok);
    void fail(const QString &code, const QString &message);
    void updateExisting(AccountState *state);
    void fetchUser();
    void addAccountAndFolder();
    void waitForCredentials(const std::function<void()> &then);

    Request _request;
    AccountPtr _account;
};

} // namespace OCC
