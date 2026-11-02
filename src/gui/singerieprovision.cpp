/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singerieprovision.h"

#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "common/utility.h"
#include "common/vfs.h"
#include "creds/abstractcredentials.h"
#include "creds/webflowcredentials.h"
#include "folder.h"
#include "folderman.h"
#include "networkjobs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QStorageInfo>
#include <QTimer>

#include <chrono>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcSingerieProvision, "nextcloud.gui.singerieprovision", QtInfoMsg)

namespace OCC
{

namespace
{

constexpr auto credentialsTimeout = std::chrono::seconds(30);

QString cleanLocal(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool samePath(const QString &a, const QString &b)
{
    return cleanLocal(a).compare(cleanLocal(b), Utility::fsCasePreserving() ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0;
}

#ifdef Q_OS_WIN
/// Whether Windows knows `path` as (inside) a cloud-files sync root, whoever registered it.
bool isInSyncRoot(const QString &path)
{
    using InfoFunction = HRESULT(WINAPI *)(LPCWSTR, int, PVOID, DWORD, DWORD *);
    static const auto info = []() -> InfoFunction {
        const auto module = LoadLibraryW(L"cldapi.dll");
        return module ? reinterpret_cast<InfoFunction>(GetProcAddress(module, "CfGetSyncRootInfoByPath")) : nullptr;
    }();
    if (!info) {
        return false;
    }
    QByteArray buffer(4096, 0);
    DWORD length = 0;
    const auto native = QDir::toNativeSeparators(path);
    const auto result = info(reinterpret_cast<const wchar_t *>(native.utf16()), 0 /* CF_SYNC_ROOT_INFO_BASIC */, buffer.data(), buffer.size(), &length);
    return SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}
#endif

} // namespace

bool SingerieProvision::parse(const QByteArray &line, Request *request, QString *error)
{
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(line.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("The request isn't one JSON object on one line.");
        return false;
    }
    const auto object = document.object();
    request->server = QUrl(object.value(QStringLiteral("server")).toString());
    request->user = object.value(QStringLiteral("user")).toString().trimmed();
    request->appPassword = object.value(QStringLiteral("app_password")).toString();
    request->localPath = object.value(QStringLiteral("local_path")).toString().trimmed();
    request->remotePath = object.value(QStringLiteral("remote_path")).toString(QStringLiteral("/"));
    request->virtualFiles = object.value(QStringLiteral("virtual_files")).toBool(true);
    request->adopt = object.value(QStringLiteral("adopt")).toBool(false);
    request->replaceUrlFrom.clear();
    for (const auto &value : object.value(QStringLiteral("replace_url_from")).toArray()) {
        request->replaceUrlFrom.append(value.toString());
    }
    const auto scheme = request->server.scheme();
    const auto host = request->server.host();
    // http only for a server on this PC (the bench), as the message says (SYNC-17): the app password never goes out
    // in the clear.
    const auto local = host == QStringLiteral("localhost") || host.endsWith(QStringLiteral(".localhost")) || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
    if (!request->server.isValid() || host.isEmpty() || !(scheme == QStringLiteral("https") || (scheme == QStringLiteral("http") && local))) {
        *error = QStringLiteral("\"server\" must be the server's https address.");
        return false;
    }
    if (request->user.isEmpty() || request->appPassword.isEmpty()) {
        *error = QStringLiteral("\"user\" and \"app_password\" are needed.");
        return false;
    }
    if (request->localPath.isEmpty() || !QDir::isAbsolutePath(request->localPath)) {
        *error = QStringLiteral("\"local_path\" must be a full path.");
        return false;
    }
    if (request->remotePath.isEmpty()) {
        request->remotePath = QStringLiteral("/");
    }
    return true;
}

QByteArray SingerieProvision::failure(const QString &code, const QString &message)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), code}, {QStringLiteral("message"), message}})
        .toJson(QJsonDocument::Compact);
}

SingerieProvision::SingerieProvision(const Request &request, QObject *parent)
    : QObject(parent)
    , _request(request)
{
}

QPair<QString, QString> SingerieProvision::checkFolder(const QString &localPath, bool adopt)
{
    const auto path = cleanLocal(localPath);
    // The nearest folder that exists tells the drive.
    auto existing = path;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const auto parent = QFileInfo(existing).absolutePath();
        if (parent == existing) {
            break;
        }
        existing = parent;
    }
#ifdef Q_OS_WIN
    const QStorageInfo storage(existing);
    if (storage.isValid() && storage.fileSystemType().toUpper() != "NTFS") {
        return {QStringLiteral("folder_not_ntfs"),
                QStringLiteral("The folder is on a %1 drive; files on demand need an NTFS drive.").arg(QString::fromLatin1(storage.fileSystemType()))};
    }
    if (QFileInfo::exists(path) && isInSyncRoot(path)) {
        return {QStringLiteral("folder_in_other_sync_root"), QStringLiteral("Another sync app still owns this folder.")};
    }
#endif
    const auto validity = FolderMan::instance()->checkPathValidityForNewFolder(path).first;
    // A folder another client synced holds its journal: refused, unless Singerie takes it over (adopt).
    if (validity == FolderMan::PathValidityResult::ErrorContainsFolder || validity == FolderMan::PathValidityResult::ErrorContainedInFolder
        || (validity == FolderMan::PathValidityResult::ErrorRecursiveValidity && !adopt)) {
        return {QStringLiteral("folder_in_other_sync_root"), QStringLiteral("The folder is inside, or holds, a folder that is already synced.")};
    }
    if (!adopt && QFileInfo(path).isDir()) {
        const auto entries = QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        const auto own = [](const QString &name) {
            return name.startsWith(QStringLiteral(".sync_")) || name.compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0;
        };
        if (std::any_of(entries.cbegin(), entries.cend(), [&own](const QString &name) {
                return !own(name);
            })) {
            return {QStringLiteral("folder_not_empty"), QStringLiteral("The folder already holds files.")};
        }
    }
    return {};
}

void SingerieProvision::finish(const QByteArray &answer, bool ok)
{
    std::cout << answer.constData() << std::endl;
    std::cout.flush();
    _request.appPassword.fill(QLatin1Char('\0'));
    _request.appPassword.clear();
    QTimer::singleShot(0, qApp, [ok] {
        qApp->exit(ok ? 0 : 1);
    });
}

void SingerieProvision::fail(const QString &code, const QString &message)
{
    qCWarning(lcSingerieProvision) << "Provisioning failed:" << code << message;
    finish(failure(code, message), false);
}

void SingerieProvision::start()
{
    FolderMan::instance()->setSyncEnabled(false);
    const auto host = _request.server.host();
    // The same account (this server's address, or an earlier one to move from): update it.
    for (const auto &state : AccountManager::instance()->accounts()) {
        const auto account = state->account();
        const auto sameUser = account->davUser().compare(_request.user, Qt::CaseInsensitive) == 0
            || account->credentials()->user().compare(_request.user, Qt::CaseInsensitive) == 0;
        if (!sameUser) {
            continue;
        }
        const auto url = account->url().toString();
        const auto moved = std::any_of(_request.replaceUrlFrom.cbegin(), _request.replaceUrlFrom.cend(), [&url](const QString &old) {
            return url.startsWith(old.endsWith(QLatin1Char('/')) ? old.chopped(1) : old, Qt::CaseInsensitive);
        });
        if (account->url().host().compare(host, Qt::CaseInsensitive) == 0 || moved) {
            updateExisting(state.data());
            return;
        }
    }
    const auto [code, message] = checkFolder(_request.localPath, _request.adopt);
    if (!code.isEmpty()) {
        fail(code, message);
        return;
    }
    fetchUser();
}

void SingerieProvision::updateExisting(AccountState *state)
{
    const auto account = state->account();
    // The same account with another folder: the sync folder moves there (Singerie's Move...: it moves the files
    // first, then provisions the new place, usually with `adopt`). The old place is handed back to Windows (its sync
    // root unregistered, its journal forgotten); its files are never touched.
    QString movedFrom;
    QList<Folder *> others;
    for (const auto folder : FolderMan::instance()->map()) {
        if (folder->accountState() == state && !samePath(folder->path(), _request.localPath)) {
            others.append(folder);
        }
    }
    if (others.size() > 1) {
        fail(QStringLiteral("account_exists"),
             QStringLiteral("This account already syncs several folders: %1.").arg(QDir::toNativeSeparators(cleanLocal(others.first()->path()))));
        return;
    }
    if (others.size() == 1) {
        const auto [code, message] = checkFolder(_request.localPath, _request.adopt);
        if (!code.isEmpty()) {
            fail(code, message);
            return;
        }
        movedFrom = QDir::toNativeSeparators(cleanLocal(others.first()->path()));
        qCInfo(lcSingerieProvision) << "The sync folder moves from" << movedFrom << "to" << QDir::toNativeSeparators(_request.localPath);
        FolderMan::instance()->removeFolder(others.first());
    }
    if (account->url() != _request.server) {
        qCInfo(lcSingerieProvision) << "The account moves to the server's new address" << _request.server.toString();
        account->setUrl(_request.server);
    }
    qCInfo(lcSingerieProvision) << "Updating the app password of" << account->displayName();
    account->setCredentials(new WebFlowCredentials(_request.user, _request.appPassword));
    _account = account;
    _movedFrom = movedFrom;
    waitForCredentials([this, state] {
        const auto folders = FolderMan::instance()->map();
        const auto hasFolder = std::any_of(folders.cbegin(), folders.cend(), [state](Folder *folder) {
            return folder->accountState() == state;
        });
        if (!hasFolder) {
            if (_movedFrom.isEmpty()) {
                const auto [code, message] = checkFolder(_request.localPath, _request.adopt);
                if (!code.isEmpty()) {
                    fail(code, message);
                    return;
                }
            }
            addAccountAndFolder();
            return;
        }
        finish(QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                         {QStringLiteral("account"), _account->displayName()},
                                         {QStringLiteral("folder"), QDir::toNativeSeparators(cleanLocal(_request.localPath))},
                                         {QStringLiteral("updated"), true}})
                   .toJson(QJsonDocument::Compact),
               true);
    });
    AccountManager::instance()->save();
}

void SingerieProvision::waitForCredentials(const std::function<void()> &then)
{
    // The credential store is written asynchronously: wait for it, but never forever.
    const auto timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(_account->credentials(), &AbstractCredentials::credentialsPersisted, this, [timer, then] {
        if (!timer->isActive()) {
            return;
        }
        timer->stop();
        then();
    });
    connect(timer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("credential_store"), QStringLiteral("Windows' credential store didn't answer; the app password wasn't saved."));
    });
    timer->start(credentialsTimeout);
}

void SingerieProvision::fetchUser()
{
    _account = AccountManager::createAccount();
    _account->setCredentials(new WebFlowCredentials(_request.user, _request.appPassword));
    _account->setCredentialSetting(QStringLiteral("user"), _request.user);
    _account->setUrl(_request.server);
    const auto job = new JsonApiJob(_account, QStringLiteral("/ocs/v1.php/cloud/user"), this);
    job->setIgnoreCredentialFailure(true);
    connect(job, &JsonApiJob::jsonReceived, this, [this](const QJsonDocument &json, int statusCode) {
        if (statusCode == 100) {
            const auto data = json.object().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toObject();
            _account->setDavUser(data.value(QStringLiteral("id")).toString(_request.user));
            _account->setDavDisplayName(data.value(QStringLiteral("display-name")).toString());
            addAccountAndFolder();
            return;
        }
        if (statusCode == 401 || statusCode == 403 || statusCode == 997) {
            fail(QStringLiteral("bad_credentials"), QStringLiteral("The server refused this user name and app password."));
        } else {
            fail(QStringLiteral("server_unreachable"), QStringLiteral("The server didn't answer (%1).").arg(statusCode));
        }
    });
    job->start();
}

void SingerieProvision::addAccountAndFolder()
{
    const auto manager = AccountManager::instance();
    const auto known = std::any_of(manager->accounts().cbegin(), manager->accounts().cend(), [this](const AccountStatePtr &state) {
        return state->account() == _account;
    });
    const auto state = known ? manager->account(_account->displayName()).data() : manager->addAccount(_account);
    const auto addFolder = [this, state] {
        QDir localDir(_request.localPath);
        if (!localDir.exists() && !localDir.mkpath(QStringLiteral("."))) {
            AccountManager::instance()->removeAccountState(state);
            fail(QStringLiteral("bad_request"), QStringLiteral("The folder couldn't be made."));
            return;
        }
        FolderDefinition definition;
        definition.localPath = _request.localPath;
        definition.targetPath = FolderDefinition::prepareTargetPath(_request.remotePath);
        definition.virtualFilesMode = _request.virtualFiles ? bestAvailableVfsMode() : Vfs::Off;
        const auto folderMan = FolderMan::instance();
        definition.ignoreHiddenFiles = folderMan->ignoreHiddenFiles();
        definition.alias = QString::number(folderMan->map().size());
        // No Explorer navigation-pane entry, no favourite link: the member goes in through Singerie's drive.
        folderMan->setSyncEnabled(false);
        // An earlier journal for this account and folder is removed by FolderMan::addFolder; one still open (another
        // program, a client still running there) would make it ask in a window nobody sees (SYNC-3): say so instead.
        {
            auto journalDefinition = definition;
            journalDefinition.journalPath = journalDefinition.defaultJournalPath(state->account());
            const auto journal = journalDefinition.absoluteJournalPath();
            if (QFile::exists(journal) && !QFile::remove(journal)) {
                AccountManager::instance()->removeAccountState(state);
                fail(QStringLiteral("journal_locked"),
                     QStringLiteral("The sync folder's old journal is still open in another program: close it and try again."));
                return;
            }
        }
        const auto folder = folderMan->addFolder(state, definition);
        if (!folder) {
            AccountManager::instance()->removeAccountState(state);
            fail(QStringLiteral("folder_in_other_sync_root"), QStringLiteral("The sync folder couldn't be set up there."));
            return;
        }
        if (definition.virtualFilesMode != Vfs::Off && !_request.adopt) {
            // A new folder starts with everything online only. An adopted folder keeps every file's pin as it was
            // (setting the root's pin reaches everything below it: "always keep on this device" was lost).
            folder->setRootPinState(PinState::OnlineOnly);
        }
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, QStringList() << QLatin1String("/"));
        qCInfo(lcSingerieProvision) << "Provisioned" << _account->displayName() << "syncing" << QDir::toNativeSeparators(_request.localPath)
                                    << (_request.adopt ? "(adopting what is there)" : "");
        auto answer = QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("account"), _account->displayName()},
                                  {QStringLiteral("folder"), QDir::toNativeSeparators(cleanLocal(_request.localPath))}};
        if (!_movedFrom.isEmpty()) {
            answer.insert(QStringLiteral("updated"), true);
            answer.insert(QStringLiteral("moved_from"), _movedFrom);
        }
        finish(QJsonDocument(answer).toJson(QJsonDocument::Compact), true);
    };
    if (known) {
        addFolder();
        return;
    }
    waitForCredentials(addFolder);
    manager->save();
}

} // namespace OCC
