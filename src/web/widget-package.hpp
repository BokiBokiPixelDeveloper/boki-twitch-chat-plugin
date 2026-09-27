#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

enum class WidgetCompatibility { Auto, GenericWebWidget, StreamElements };

struct WidgetFile {
    QString relativePath;
    QByteArray sha256;
    quint64 size = 0;
};

struct WidgetEntrypoints {
    QString html;
    QString css;
    QString javascript;
    QString fields;
    QString data;
};

struct WidgetPackage {
    QString id;
    QString displayName;
    QByteArray archiveSha256;
    QDateTime importedAt;
    QString originalRoot;
    std::vector<WidgetFile> files;
    WidgetEntrypoints entrypoints;
    WidgetCompatibility detectedCompatibility = WidgetCompatibility::GenericWebWidget;
    QStringList detectionEvidence;
    QJsonObject fieldDefaults;
    QJsonObject savedFieldValues;
};

[[nodiscard]] QString widgetCompatibilityName(WidgetCompatibility compatibility);
[[nodiscard]] WidgetCompatibility widgetCompatibilityFromName(const QString &name);
[[nodiscard]] WidgetCompatibility detectWidgetCompatibility(const WidgetEntrypoints &entrypoints,
                                                             const QString &root, QStringList *evidence = nullptr);

class WidgetPackageStore final {
public:
    explicit WidgetPackageStore(QString root);

    [[nodiscard]] QString root() const { return root_; }
    [[nodiscard]] std::shared_ptr<const WidgetPackage> package(const QString &id) const;
    [[nodiscard]] std::vector<std::shared_ptr<const WidgetPackage>> packages() const;
    [[nodiscard]] bool install(WidgetPackage package, const QString &stagingRoot, QString *error = nullptr);
    [[nodiscard]] bool removeIncomplete(const QString &stagingRoot) const;

private:
    [[nodiscard]] std::shared_ptr<const WidgetPackage> load(const QString &id) const;
    QString root_;
};
