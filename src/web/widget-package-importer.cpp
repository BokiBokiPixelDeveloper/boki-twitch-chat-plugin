#include "web/widget-package-importer.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

namespace {
bool safePath(const QString &path, const WidgetPackageImporter::Limits &limits)
{
    if (path.isEmpty() || path.toUtf8().size() > limits.maximumPathBytes || path.startsWith('/') || path.startsWith('\\') ||
        path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    const auto parts = path.split('/');
    if (parts.size() > limits.maximumDepth) return false;
    for (const auto &part : parts)
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.toUtf8().size() > limits.maximumComponentBytes || part.endsWith('.') || part.endsWith(' ')) return false;
    return QDir::cleanPath(path) == path;
}

QString errorFromArchive(archive *reader) { return QString::fromUtf8(archive_error_string(reader) ? archive_error_string(reader) : "Malformed ZIP archive"); }
QString collisionKey(const QString &path) { return path.normalized(QString::NormalizationForm_C).toCaseFolded(); }
}

WidgetPackageImporter::WidgetPackageImporter(WidgetPackageStore &store) : WidgetPackageImporter(store, Limits{}) {}
WidgetPackageImporter::WidgetPackageImporter(WidgetPackageStore &store, Limits limits) : store_(store), limits_(limits) {}

std::shared_ptr<const WidgetPackage> WidgetPackageImporter::importArchive(const QString &archivePath, QString *error)
{
    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.isFile() || archiveInfo.size() < 1 || quint64(archiveInfo.size()) > limits_.maximumArchiveBytes) {
        if (error) *error = QStringLiteral("Widget archive is missing or exceeds the size limit");
        return {};
    }
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) { if (error) *error = QStringLiteral("Could not read widget archive"); return {}; }
    const QByteArray archiveHash = QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256);
    const QString id = QString::fromLatin1(archiveHash.toHex());
    if (auto existing = store_.package(id)) return existing;

    QTemporaryDir staging(QDir(store_.root()).filePath(QStringLiteral(".import-XXXXXX")));
    if (!staging.isValid()) { if (error) *error = QStringLiteral("Could not create secure import staging"); return {}; }
    const QString originalRoot = QDir(staging.path()).filePath(QStringLiteral("original"));
    if (!QDir().mkpath(originalRoot)) { if (error) *error = QStringLiteral("Could not create package staging"); return {}; }

    archive *reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
    if (archive_read_open_filename(reader, QFile::encodeName(archivePath).constData(), 64 * 1024) != ARCHIVE_OK) {
        if (error) *error = errorFromArchive(reader);
        archive_read_free(reader);
        return {};
    }
    WidgetPackage package;
    package.id = id; package.archiveSha256 = archiveHash; package.importedAt = QDateTime::currentDateTimeUtc();
    package.displayName = archiveInfo.completeBaseName(); package.originalRoot = originalRoot;
    QSet<QString> names, fileNames;
    quint64 total = 0;
    archive_entry *entry = nullptr;
    bool failed = false;
    QString reason;
    int fileCount = 0;
    int headerStatus = ARCHIVE_OK;
    while ((headerStatus = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        QString path = QString::fromUtf8(archive_entry_pathname(entry));
        const auto kind = archive_entry_filetype(entry);
        const auto declaredSize = archive_entry_size(entry);
        const bool directory = kind == AE_IFDIR;
        if (directory && path.endsWith('/')) path.chop(1);
        const QString key = collisionKey(path);
        bool prefixConflict = false;
        QString ancestor;
        for (const auto &part : path.split('/')) {
            if (!ancestor.isEmpty()) ancestor += QLatin1Char('/');
            ancestor += part;
            if (ancestor != path && fileNames.contains(collisionKey(ancestor))) prefixConflict = true;
        }
        if (!directory)
            for (const auto &known : names) if (known.startsWith(key + QLatin1Char('/'))) prefixConflict = true;
        if ((!directory && kind != AE_IFREG) || !safePath(path, limits_) || archive_entry_hardlink(entry) ||
            archive_entry_symlink(entry) || archive_entry_is_encrypted(entry) == 1 || declaredSize < 0 ||
            quint64(declaredSize) > limits_.maximumFileBytes || ++fileCount > limits_.maximumFiles || names.contains(key) ||
            prefixConflict || total > limits_.maximumExpandedBytes - quint64(declaredSize)) {
            failed = true; reason = QStringLiteral("Widget archive contains an unsafe entry"); break;
        }
        names.insert(key);
        if (!directory) fileNames.insert(key);
        if (directory) {
            if (!QDir().mkpath(QDir(originalRoot).filePath(path))) { failed = true; reason = QStringLiteral("Could not create package directory"); break; }
            continue;
        }
        total += quint64(declaredSize);
        const auto compressed = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : 0;
        Q_UNUSED(compressed);
        const QString destination = QDir(originalRoot).filePath(path);
        if (!QDir().mkpath(QFileInfo(destination).dir().path())) { failed = true; reason = QStringLiteral("Could not create package path"); break; }
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { failed = true; reason = QStringLiteral("Duplicate package path"); break; }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        quint64 written = 0;
        char buffer[64 * 1024];
        la_ssize_t read = 0;
        while ((read = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
            if (quint64(read) > limits_.maximumFileBytes - written || output.write(buffer, read) != read) {
                failed = true; reason = QStringLiteral("Widget file exceeds extraction limit"); break;
            }
            written += quint64(read); hash.addData(QByteArrayView(buffer, read));
        }
        output.close();
        if (read < 0) { failed = true; reason = errorFromArchive(reader); }
        if (failed || written != quint64(declaredSize)) { failed = true; if (reason.isEmpty()) reason = QStringLiteral("Widget archive has a malformed entry"); break; }
        package.files.push_back({path, hash.result(), written});
    }
    if (headerStatus != ARCHIVE_EOF && !failed) { failed = true; reason = errorFromArchive(reader); }
    if (archive_read_free(reader) != ARCHIVE_OK && !failed) { failed = true; reason = QStringLiteral("Could not finalize widget archive"); }
    if (!failed && total > quint64(archiveInfo.size()) * quint64(limits_.maximumCompressionRatio)) {
        failed = true;
        reason = QStringLiteral("Widget archive exceeds compression expansion limit");
    }
    if (failed || package.files.empty()) { if (error) *error = failed ? reason : QStringLiteral("Widget archive is empty"); return {}; }

    const auto find = [&](const QString &name) { return std::find_if(package.files.cbegin(), package.files.cend(), [&](const WidgetFile &file) { return file.relativePath == name; }) != package.files.cend(); };
    QFile ini(QDir(originalRoot).filePath(QStringLiteral("widget.ini")));
    if (ini.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(ini.readAll());
        // Release the staging file before install() renames its directory.
        // An open QFile prevents that rename on Windows.
        ini.close();
        const auto role = [&](const QString &section) {
            const QRegularExpression expression(QStringLiteral("\\[%1\\]\\s*\\n\\s*path\\s*=\\s*\\\"?([^\\\"\\r\\n]+)").arg(section));
            const auto match = expression.match(text); return match.hasMatch() ? match.captured(1).trimmed() : QString{};
        };
        package.entrypoints = {role("HTML"), role("CSS"), role("JS"), role("FIELDS"), role("DATA")};
    } else {
        package.entrypoints.html = find(QStringLiteral("index.html")) ? QStringLiteral("index.html") : QString{};
    }
    for (const auto &path : {package.entrypoints.html, package.entrypoints.css, package.entrypoints.javascript, package.entrypoints.fields, package.entrypoints.data})
        if (!path.isEmpty() && !find(path)) { if (error) *error = QStringLiteral("Widget manifest references a missing file"); return {}; }
    if (package.entrypoints.html.isEmpty()) { if (error) *error = QStringLiteral("Widget has no HTML entrypoint"); return {}; }
    for (const auto &[path, target] : std::initializer_list<std::pair<QString, QJsonObject *>>{{package.entrypoints.fields, &package.fieldDefaults}, {package.entrypoints.data, &package.savedFieldValues}}) {
        if (path.isEmpty()) continue;
        QFile json(QDir(originalRoot).filePath(path));
        if (json.open(QIODevice::ReadOnly)) {
            const auto object = QJsonDocument::fromJson(json.readAll()).object();
            if (!object.isEmpty()) *target = object;
        }
    }
    package.detectedCompatibility = detectWidgetCompatibility(package.entrypoints, originalRoot, &package.detectionEvidence);
    staging.setAutoRemove(false);
    if (!store_.install(std::move(package), staging.path(), error)) {
        static_cast<void>(store_.removeIncomplete(staging.path()));
        return {};
    }
    return store_.package(id);
}
