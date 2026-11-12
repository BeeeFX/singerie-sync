/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "singeriecrash.h"

#include "configfile.h"
#include "singeriesettings.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dbghelp.h>

#include <cwchar>
#endif

namespace OCC {

Q_LOGGING_CATEGORY(lcSingerieCrash, "nextcloud.sync.singeriecrash", QtInfoMsg)

namespace {

#ifdef Q_OS_WIN
using WriteDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                 PMINIDUMP_CALLBACK_INFORMATION);
WriteDump writeDump = nullptr;
wchar_t dumpFolder[MAX_PATH] = {};

/// Runs in the crashing process: nothing but Windows calls on what was made ready at start.
LONG WINAPI writeCrashFile(EXCEPTION_POINTERS *info)
{
    if (!writeDump || dumpFolder[0] == L'\0') {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH + 64];
    swprintf(path,
             sizeof(path) / sizeof(path[0]),
             L"%ls\\singeriesync-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
             dumpFolder,
             now.wYear,
             now.wMonth,
             now.wDay,
             now.wHour,
             now.wMinute,
             now.wSecond,
             GetCurrentProcessId());
    const auto file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        // The threads' stacks and the loaded modules only: no file contents, no heap.
        writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH; // Windows' own handling goes on (its Application log line)
}
#endif

} // namespace

QString SingerieCrash::folder()
{
    return ConfigFile().configPath() + QStringLiteral("crashes");
}

QStringList SingerieCrash::prune(const QString &folder, int count)
{
    QStringList deleted;
    const auto dumps = QDir(folder).entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time); // newest first
    for (auto i = count; i < dumps.size(); ++i) {
        if (QFile::remove(dumps.at(i).absoluteFilePath())) {
            deleted.append(dumps.at(i).fileName());
        }
    }
    return deleted;
}

bool SingerieCrash::install()
{
#ifdef Q_OS_WIN
    if (!SingerieSettings::crashDumps()) {
        return false;
    }
    const auto where = folder();
    if (!QDir().mkpath(where)) {
        qCWarning(lcSingerieCrash) << "Could not make the crash files' folder" << where;
        return false;
    }
    prune(where, 2);
    const auto native = QDir::toNativeSeparators(where).toStdWString();
    if (native.size() >= MAX_PATH) {
        return false;
    }
    wcsncpy_s(dumpFolder, native.c_str(), _TRUNCATE);
    if (const auto module = LoadLibraryW(L"dbghelp.dll")) {
        writeDump = reinterpret_cast<WriteDump>(GetProcAddress(module, "MiniDumpWriteDump"));
    }
    if (!writeDump) {
        qCWarning(lcSingerieCrash) << "No MiniDumpWriteDump on this PC: no crash files";
        return false;
    }
    SetUnhandledExceptionFilter(writeCrashFile);
    qCInfo(lcSingerieCrash) << "A crash file is kept in" << where << "(three at most, never sent)";
    return true;
#else
    return false;
#endif
}

} // namespace OCC
