#pragma once

#include "web/widget-package.hpp"

#include <QString>

class WidgetPackageImporter final {
public:
    struct Limits {
        quint64 maximumArchiveBytes = 32ULL * 1024 * 1024;
        quint64 maximumExpandedBytes = 128ULL * 1024 * 1024;
        quint64 maximumFileBytes = 16ULL * 1024 * 1024;
        int maximumFiles = 2048;
        int maximumDepth = 16;
        int maximumPathBytes = 512;
        int maximumComponentBytes = 128;
        int maximumCompressionRatio = 200;
    };

    explicit WidgetPackageImporter(WidgetPackageStore &store);
    WidgetPackageImporter(WidgetPackageStore &store, Limits limits);
    [[nodiscard]] std::shared_ptr<const WidgetPackage> importArchive(const QString &archivePath, QString *error = nullptr);

private:
    WidgetPackageStore &store_;
    Limits limits_;
};
