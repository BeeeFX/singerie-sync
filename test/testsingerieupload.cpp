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

namespace
{

void wholeUploads(FakeFolder &fakeFolder)
{
    fakeFolder.syncEngine().account()->setCapabilities({{"dav", QVariantMap{{"chunking", "1.0"}}}});
    auto options = fakeFolder.syncEngine().syncOptions();
    options._wholeUploadAge = std::chrono::seconds(60);
    fakeFolder.syncEngine().setSyncOptions(options);
}

} // namespace

class TestSingerieUpload : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    /*
     * A10: a file still growing while it goes up is never left truncated on the server. A file changed in the last
     * minute goes up in chunks; if it changed meanwhile, the server doesn't put it in place, and the next sync sends
     * it whole.
     */
    void testGrowingFileIsNeverTruncatedOnTheServer()
    {
        FakeFolder fakeFolder{FileInfo{}};
        wholeUploads(fakeFolder);
        fakeFolder.localModifier().mkdir(QStringLiteral("render"));
        fakeFolder.localModifier().insert(QStringLiteral("render/frame.0001.exr"), 100);
        auto grown = false;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            // The renderer writes more while the first chunk goes up.
            if (op == QNetworkAccessManager::PutOperation && request.url().path().contains(QStringLiteral("/uploads/")) && !grown) {
                grown = true;
                fakeFolder.localModifier().appendByte(QStringLiteral("render/frame.0001.exr"));
            }
            return nullptr;
        });
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(grown);
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("render/frame.0001.exr"))); // nothing truncated in place

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState().find(QStringLiteral("render/frame.0001.exr"))->size, 101);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    /*
     * A file changed long ago goes up in one request, as upstream's (no chunk folder): frames rendered earlier stay
     * fast.
     */
    void testOldFileGoesUpInOneRequest()
    {
        FakeFolder fakeFolder{FileInfo{}};
        wholeUploads(fakeFolder);
        auto chunkFolders = 0;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.attribute(QNetworkRequest::CustomVerbAttribute).toString() == QStringLiteral("MKCOL")
                && request.url().path().contains(QStringLiteral("/uploads/"))) {
                ++chunkFolders;
            }
            return nullptr;
        });
        fakeFolder.localModifier().insert(QStringLiteral("old.exr"), 100);
        fakeFolder.localModifier().setModTime(QStringLiteral("old.exr"), QDateTime::currentDateTimeUtc().addSecs(-3600));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(chunkFolders, 0);

        fakeFolder.localModifier().insert(QStringLiteral("new.exr"), 100);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(chunkFolders, 1);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }
};

QTEST_GUILESS_MAIN(TestSingerieUpload)
#include "testsingerieupload.moc"
