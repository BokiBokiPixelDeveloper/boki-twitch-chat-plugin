#include "web/widget-package-importer.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>

namespace {
bool writeArchive(const QString &path, const std::vector<std::pair<QString, QByteArray>> &files)
{
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
    for (const auto &[name, content] : files) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, AE_IFREG); archive_entry_set_perm(entry, 0644); archive_entry_set_size(entry, content.size());
        if (archive_write_header(writer, entry) != ARCHIVE_OK || archive_write_data(writer, content.constData(), content.size()) != content.size()) {
            archive_entry_free(entry); archive_write_free(writer); return false;
        }
        archive_entry_free(entry);
    }
    return archive_write_close(writer) == ARCHIVE_OK && archive_write_free(writer) == ARCHIVE_OK;
}
bool writeSymlinkArchive(const QString &path)
{
    archive *writer = archive_write_new(); archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
    archive_entry *entry = archive_entry_new(); archive_entry_set_pathname(entry, "link");
    archive_entry_set_filetype(entry, AE_IFLNK); archive_entry_set_perm(entry, 0777); archive_entry_set_symlink(entry, "/etc/passwd");
    const bool ok = archive_write_header(writer, entry) == ARCHIVE_OK;
    archive_entry_free(entry); return archive_write_close(writer) == ARCHIVE_OK && archive_write_free(writer) == ARCHIVE_OK && ok;
}
}

class WidgetPackageTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void importsAndDetectsStreamElements()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("widget.zip");
        QVERIFY(writeArchive(path, {{"widget.ini", "[HTML]\npath=html.txt\n[CSS]\npath=css.txt\n[JS]\npath=js.txt\n[FIELDS]\npath=fields.txt\n[DATA]\npath=data.txt\n"},
            {"html.txt", "<div></div>"}, {"css.txt", "body{}"}, {"js.txt", "addEventListener('onWidgetLoad',()=>fieldData); addEventListener('onEventReceived',()=>{});"},
            {"fields.txt", "{\"enabled\":{\"type\":\"checkbox\",\"value\":true}}"}, {"data.txt", "{\"enabled\":false}"}}));
        WidgetPackageStore store(temporary.filePath("store")); WidgetPackageImporter importer(store);
        QString error; const auto package = importer.importArchive(path, &error);
        QVERIFY2(package, qPrintable(error)); QCOMPARE(package->detectedCompatibility, WidgetCompatibility::StreamElements);
        QCOMPARE(package->fieldDefaults.value("enabled").toObject().value("value").toBool(), true);
        QVERIFY(store.package(package->id));
        const auto original = QFile(QDir(package->originalRoot).filePath("js.txt")); QVERIFY(original.exists());
        const auto again = importer.importArchive(path, &error); QVERIFY(again); QCOMPARE(again->id, package->id);
        QCOMPARE(QDir(store.root()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{package->id});
    }
    void importsWithoutManifest()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("widget.zip");
        QVERIFY(writeArchive(path, {{"index.html", "<div>Widget</div>"}}));
        WidgetPackageStore store(temporary.filePath("store")); WidgetPackageImporter importer(store);
        QString error; const auto package = importer.importArchive(path, &error);
        QVERIFY2(package, qPrintable(error));
        QCOMPARE(package->entrypoints.html, QStringLiteral("index.html"));
        QCOMPARE(package->detectedCompatibility, WidgetCompatibility::GenericWebWidget);
        QVERIFY(store.package(package->id));
        QCOMPARE(QDir(store.root()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{package->id});
    }
    void rejectsUnsafePathsAndLeavesNoPackage()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        WidgetPackageStore store(temporary.filePath("store")); WidgetPackageImporter importer(store);
        for (const QString &unsafe : {QStringLiteral("../escape.txt"), QStringLiteral("/absolute.txt"), QStringLiteral("nested\\escape.txt"), QStringLiteral("one.txt")}) {
            const QString path = temporary.filePath(unsafe == "one.txt" ? "duplicate.zip" : "unsafe.zip");
            const auto files = unsafe == "one.txt" ? std::vector<std::pair<QString, QByteArray>>{{"one.txt", "a"}, {"ONE.txt", "b"}}
                                                : std::vector<std::pair<QString, QByteArray>>{{unsafe, "bad"}};
            QVERIFY(writeArchive(path, files)); QString error; QVERIFY(!importer.importArchive(path, &error)); QVERIFY(!error.isEmpty());
            QCOMPARE(store.packages().size(), size_t(0));
        }
    }
    void importsReadOnlyScrapbookFixtureWhenAvailable()
    {
        if (!QFile::exists(QStringLiteral(SCRAPBOOK_FIXTURE))) QSKIP("Local Scrapbook fixture is unavailable");
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); WidgetPackageStore store(temporary.filePath("store")); WidgetPackageImporter importer(store);
        QString error; const auto package = importer.importArchive(QStringLiteral(SCRAPBOOK_FIXTURE), &error);
        QVERIFY2(package, qPrintable(error)); QCOMPARE(package->detectedCompatibility, WidgetCompatibility::StreamElements);
        QCOMPARE(package->files.size(), size_t(6)); QVERIFY(package->entrypoints.javascript == QStringLiteral("js.txt"));
    }
    void rejectsSymlinksLimitsMalformedAndPrefixCollisions()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); WidgetPackageStore store(temporary.filePath("store"));
        QString error;
        const QString symlink = temporary.filePath("symlink.zip"); QVERIFY(writeSymlinkArchive(symlink));
        WidgetPackageImporter importer(store); QVERIFY(!importer.importArchive(symlink, &error));
        const QString prefix = temporary.filePath("prefix.zip"); QVERIFY(writeArchive(prefix, {{"folder", "file"}, {"folder/index.html", "html"}}));
        QVERIFY(!importer.importArchive(prefix, &error));
        const QString count = temporary.filePath("count.zip"); QVERIFY(writeArchive(count, {{"index.html", "html"}, {"extra.txt", "extra"}}));
        WidgetPackageImporter::Limits limits; limits.maximumFiles = 1; WidgetPackageImporter limited(store, limits);
        QVERIFY(!limited.importArchive(count, &error));
        const QString malformed = temporary.filePath("malformed.zip"); QFile bad(malformed); QVERIFY(bad.open(QIODevice::WriteOnly));
        QCOMPARE(bad.write("not a zip"), qint64(9)); bad.close(); QVERIFY(!importer.importArchive(malformed, &error));
        QCOMPARE(store.packages().size(), size_t(0));
        for (const auto &entry : QDir(store.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) QVERIFY(!entry.startsWith(".import-"));
    }
};
QTEST_GUILESS_MAIN(WidgetPackageTests)
#include "widget-package-tests.moc"
