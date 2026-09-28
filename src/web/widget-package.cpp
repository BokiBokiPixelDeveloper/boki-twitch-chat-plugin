#include "web/widget-package.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace {
QString packageDirectory(const QString &root, const QString &id)
{
    return QDir(root).filePath(id);
}

QJsonObject serialize(const WidgetPackage &package)
{
    QJsonArray files;
    for (const auto &file : package.files)
        files.append(QJsonObject{{"path", file.relativePath}, {"sha256", QString::fromLatin1(file.sha256.toHex())},
                                 {"size", QString::number(file.size)}});
    return {{"schemaVersion", 1}, {"id", package.id}, {"displayName", package.displayName},
            {"archiveSha256", QString::fromLatin1(package.archiveSha256.toHex())},
            {"importedAt", package.importedAt.toUTC().toString(Qt::ISODateWithMs)},
            {"compatibility", widgetCompatibilityName(package.detectedCompatibility)},
            {"detectionEvidence", QJsonArray::fromStringList(package.detectionEvidence)}, {"files", files},
            {"entrypoints", QJsonObject{{"html", package.entrypoints.html}, {"css", package.entrypoints.css},
                                          {"javascript", package.entrypoints.javascript}, {"fields", package.entrypoints.fields},
                                          {"data", package.entrypoints.data}}},
            {"fieldDefaults", package.fieldDefaults}, {"savedFieldValues", package.savedFieldValues}};
}

std::optional<WidgetPackage> parse(const QString &root, const QByteArray &bytes)
{
    const auto object = QJsonDocument::fromJson(bytes).object();
    if (object.value("schemaVersion").toInt() != 1) return std::nullopt;
    WidgetPackage package;
    package.id = object.value("id").toString();
    if (package.id.size() != 64 || !std::all_of(package.id.cbegin(), package.id.cend(), [](QChar c) {
            return c.isDigit() || (c >= u'a' && c <= u'f'); })) return std::nullopt;
    package.displayName = object.value("displayName").toString();
    package.archiveSha256 = QByteArray::fromHex(object.value("archiveSha256").toString().toLatin1());
    package.importedAt = QDateTime::fromString(object.value("importedAt").toString(), Qt::ISODateWithMs).toUTC();
    package.detectedCompatibility = widgetCompatibilityFromName(object.value("compatibility").toString());
    for (const auto &value : object.value("detectionEvidence").toArray()) package.detectionEvidence.append(value.toString());
    const auto entries = object.value("entrypoints").toObject();
    package.entrypoints = {entries.value("html").toString(), entries.value("css").toString(), entries.value("javascript").toString(),
                           entries.value("fields").toString(), entries.value("data").toString()};
    package.fieldDefaults = object.value("fieldDefaults").toObject();
    package.savedFieldValues = object.value("savedFieldValues").toObject();
    package.originalRoot = QDir(root).filePath(package.id + QStringLiteral("/original"));
    for (const auto &value : object.value("files").toArray()) {
        const auto file = value.toObject();
        const auto path = file.value("path").toString();
        const auto hash = QByteArray::fromHex(file.value("sha256").toString().toLatin1());
        bool validSize = false;
        const auto size = file.value("size").toString().toULongLong(&validSize);
        if (path.isEmpty() || hash.size() != 32 || !validSize) return std::nullopt;
        package.files.push_back({path, hash, size});
    }
    return package;
}
}

QString widgetCompatibilityName(WidgetCompatibility compatibility)
{
    switch (compatibility) {
    case WidgetCompatibility::Auto: return QStringLiteral("auto");
    case WidgetCompatibility::GenericWebWidget: return QStringLiteral("generic");
    case WidgetCompatibility::StreamElements: return QStringLiteral("streamelements");
    }
    return QStringLiteral("generic");
}

WidgetCompatibility widgetCompatibilityFromName(const QString &name)
{
    if (name == QStringLiteral("auto")) return WidgetCompatibility::Auto;
    if (name == QStringLiteral("streamelements")) return WidgetCompatibility::StreamElements;
    return WidgetCompatibility::GenericWebWidget;
}

WidgetCompatibility detectWidgetCompatibility(const WidgetEntrypoints &entrypoints, const QString &root, QStringList *evidence)
{
    QString source;
    for (const auto &path : {entrypoints.javascript, entrypoints.fields, entrypoints.data}) {
        QFile file(QDir(root).filePath(path));
        if (file.open(QIODevice::ReadOnly)) source += QString::fromUtf8(file.read(1024 * 1024));
    }
    QStringList found;
    if (!entrypoints.html.isEmpty() && !entrypoints.css.isEmpty() && !entrypoints.javascript.isEmpty()) found << QStringLiteral("widget.ini role mapping");
    if (source.contains(QStringLiteral("onWidgetLoad"))) found << QStringLiteral("onWidgetLoad");
    if (source.contains(QStringLiteral("onEventReceived"))) found << QStringLiteral("onEventReceived");
    if (source.contains(QStringLiteral("fieldData"))) found << QStringLiteral("fieldData");
    if (source.contains(QStringLiteral("SE_API"))) found << QStringLiteral("SE_API");
    if (evidence) *evidence = found;
    return found.contains(QStringLiteral("onWidgetLoad")) && found.contains(QStringLiteral("onEventReceived")) &&
                   (found.contains(QStringLiteral("fieldData")) || found.contains(QStringLiteral("SE_API")))
        ? WidgetCompatibility::StreamElements : WidgetCompatibility::GenericWebWidget;
}

WidgetPackageStore::WidgetPackageStore(QString root) : root_(std::move(root)) { QDir().mkpath(root_); }

std::shared_ptr<const WidgetPackage> WidgetPackageStore::load(const QString &id) const
{
    QFile file(QDir(packageDirectory(root_, id)).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto parsed = parse(root_, file.readAll());
    if (!parsed) return {};
    for (const auto &entry : parsed->files) {
        QFile original(QDir(parsed->originalRoot).filePath(entry.relativePath));
        if (!original.open(QIODevice::ReadOnly) || original.size() != qint64(entry.size) ||
            QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256) != entry.sha256) return {};
    }
    return std::make_shared<const WidgetPackage>(std::move(*parsed));
}

std::shared_ptr<const WidgetPackage> WidgetPackageStore::package(const QString &id) const { return load(id); }

std::vector<std::shared_ptr<const WidgetPackage>> WidgetPackageStore::packages() const
{
    std::vector<std::shared_ptr<const WidgetPackage>> result;
    for (const auto &directory : QDir(root_).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (auto value = load(directory)) result.push_back(std::move(value));
    return result;
}

bool WidgetPackageStore::install(WidgetPackage package, const QString &stagingRoot, QString *error)
{
    const QString destination = packageDirectory(root_, package.id);
    if (auto existing = load(package.id)) return true;
    const QString original = QDir(stagingRoot).filePath(QStringLiteral("original"));
    if (!QFileInfo::exists(original)) { if (error) *error = QStringLiteral("Import staging is incomplete"); return false; }
    package.originalRoot = QDir(destination).filePath(QStringLiteral("original"));
    QSaveFile manifest(QDir(stagingRoot).filePath(QStringLiteral("manifest.json")));
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(QJsonDocument(serialize(package)).toJson(QJsonDocument::Compact)) < 0 || !manifest.commit()) {
        if (error) *error = QStringLiteral("Could not write package manifest");
        return false;
    }
    if (!QDir().rename(stagingRoot, destination)) {
        if (error) *error = QStringLiteral("Could not install package atomically");
        return false;
    }
    return bool(load(package.id));
}

bool WidgetPackageStore::removeIncomplete(const QString &stagingRoot) const { return QDir(stagingRoot).removeRecursively(); }
