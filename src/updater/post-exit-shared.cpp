#include "updater/post-exit-internal.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>
#include <vector>

namespace postexit {
using namespace detail;
namespace detail {
void verify(QFile &file, const PendingFile &p)
{
    require(file.size() == p.size, "File size does not match");
    require(file.seek(0) && validBinary(file.peek(4096)), "Invalid platform binary format");
    require(file.seek(0), "Pending file is not readable");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file) && QString::fromLatin1(hash.result().toHex()) == p.sha256.toLower(), "SHA-256 does not match");
}
}
bool writePending(const QString &directory, const Pending &p, QString &error)
{
    try {
        QJsonObject object{{"schema", p.helper ? 2 : 1}, {"fromVersion", p.fromVersion}, {"version", p.version},
            {"sha256", p.sha256}, {"size", p.size}, {"binary", p.binary}, {"target", p.target}};
        if (p.helper) object["helper"] = QJsonObject{{"sha256", p.helper->sha256}, {"size", p.helper->size},
            {"binary", p.helper->binary}, {"target", p.helper->target}};
        saveJson(directory + "/pending.json", object);
        return true;
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
}
Pending readPending(const QString &directory)
{
    QFile file(directory + "/pending.json");
    require(file.open(QIODevice::ReadOnly), "Pending metadata is missing");
    const auto p = QJsonDocument::fromJson(file.readAll()).object();
    Pending result{p["fromVersion"].toString(), p["version"].toString(), p["sha256"].toString(),
                   p["binary"].toString(), p["target"].toString(), p["size"].toInteger(-1), std::nullopt};
    const int schema = p["schema"].toInt();
    require((schema == 1 || schema == 2) && !result.version.isEmpty() && result.sha256.size() == 64 && result.size > 0 &&
            QDir::isAbsolutePath(result.target) && result.binary == directory + "/" + pluginFileName() &&
            QFileInfo(result.target).fileName() == pluginFileName(), "Invalid pending metadata");
    require((schema == 2) == p.contains("helper"), "Invalid pending schema for helper update");
    if (schema == 2) {
        const auto h = p["helper"].toObject();
        result.helper = PendingFile{h["sha256"].toString(), h["binary"].toString(), h["target"].toString(), h["size"].toInteger(-1)};
        require(QRegularExpression("^[0-9a-fA-F]{64}$").match(result.helper->sha256).hasMatch() && result.helper->size > 0 &&
                result.helper->binary == directory + "/" + helperFileName() &&
                result.helper->target == QFileInfo(result.target).absolutePath() + "/" + helperFileName(),
                "Invalid pending metadata for helper");
    }
    return result;
}
namespace {
void checkMappings(const QString &path, const MappingScan &scan)
{
    QString error;
    if (!ensureNotMapped(path, error, scan)) throw std::runtime_error(error.toStdString());
}
struct InstallFile {
    PendingFile file;
    QString candidate, rollback;
    bool swapped = false;
};
void restore(const InstallFile &item, const MappingScan &scan)
{
    // Keep the original rollback inode available until all restores are durable.
    require(QFileInfo(item.rollback).isFile(), "Rollback file is missing");
    if (sameFile(item.rollback, item.file.target)) return;
    if (QFileInfo(item.file.target).fileName() == pluginFileName()) checkMappings(item.file.target, scan);
    const auto restoring = item.rollback + ".restore";
    QFile::remove(restoring);
    require(linkFile(item.rollback, restoring) == 0,
            "Rollback link could not be created");
    require(renameFile(restoring, item.file.target) == 0, "Rollback rename failed; manual recovery required");
    syncDirectory(QFileInfo(item.file.target).absolutePath());
}
void cleanupTransaction(const std::vector<InstallFile> &files, const QString &journal)
{
    // Remove the journal durably first: otherwise recovery could reference deleted originals.
    if (QFile::exists(journal)) {
        require(QFile::remove(journal), "Transaction journal could not be removed");
        syncDirectory(QFileInfo(journal).absolutePath());
    }
    for (const auto &item : files) {
        QFile::remove(item.candidate);
        QFile::remove(item.rollback);
        QFile::remove(item.rollback + ".restore");
    }
}
}
bool install(const QString &directory, const QString &backups, const QString &result,
             const std::function<bool()> &waiter, QString &error, const RenameOperation &renameOperation, const MappingScan &scan)
{
    Pending p;
    const QString journalPath = directory + "/transaction.json";
    std::vector<InstallFile> files;
    bool committed = false, journalPrepared = false, rollbackFailed = false;
    Lock useLock; // Retain the target lock through rollback, result persistence and cleanup.
    try {
        p = readPending(directory);
        require(waiter(), "Process exit could not be determined safely");
        p = readPending(directory);
        useLock.value = acquireLock(p.target + ".use.lock");
        require(useLock.value >= 0, "Another OBS instance is using the plugin");
        checkMappings(p.target, scan);
        if (p.helper) files.push_back({*p.helper, {}, {}, false}); // Helper first; plugin is the final swap.
        files.push_back({{p.sha256, p.binary, p.target, p.size}, {}, {}, false});

        if (QFile::exists(journalPath)) {
            QFile journalFile(journalPath);
            require(journalFile.open(QIODevice::ReadOnly), "Transaction journal is not readable");
            const auto journal = QJsonDocument::fromJson(journalFile.readAll()).object();
            journalFile.close();
            const auto entries = journal["files"].toArray();
            const auto id = journal["id"].toString();
            require(journal["schema"].toInt() == 1 && journal["version"].toString() == p.version &&
                    QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch() &&
                    entries.size() == static_cast<qsizetype>(files.size()), "Invalid transaction journal");
            for (size_t i = 0; i < files.size(); ++i) {
                auto &item = files[i];
                const auto entry = entries[static_cast<qsizetype>(i)].toObject();
                require(entry["target"].toString() == item.file.target && entry["sha256"].toString() == item.file.sha256,
                        "Transaction journal belongs to a different update");
                item.candidate = item.file.target + ".update-" + id;
                item.rollback = item.file.target + ".rollback-" + id;
            }
            const auto phase = journal["phase"].toString();
            require(phase == "prepared" || phase == "committed", "Invalid transaction state");
            journalPrepared = true;
            if (phase == "committed") {
                for (const auto &item : files) {
                    QFile installed(item.file.target);
                    require(installed.open(QIODevice::ReadOnly), "Installed transaction is not readable");
                    verify(installed, item.file);
                }
                committed = true; // Finish an interrupted result/state cleanup without installing twice.
            } else {
                for (auto it = files.rbegin(); it != files.rend(); ++it) {
                    if (it->file.target == p.target) checkMappings(p.target, scan);
                    restore(*it, scan);
                }
                cleanupTransaction(files, journalPath);
                journalPrepared = false;
            }
        }
        if (!committed) {
            // Verify EVERY payload before preparing or exchanging either installed file.
            for (const auto &item : files) {
                QFile input(item.file.binary);
                require(!QFileInfo(item.file.binary).isSymLink() && input.open(QIODevice::ReadOnly), "Pending file is missing or unreadable");
                verify(input, item.file);
                require(!QFileInfo(item.file.target).isSymLink(), "Installation target must not be a symbolic link");
            }
            require(QDir().mkpath(backups), "Backup directory could not be created");
            const auto id = QUuid::createUuid().toString(QUuid::Id128);
            const auto stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz") + "-" + id;
            QJsonArray entries;
            for (auto &item : files) {
                item.candidate = item.file.target + ".update-" + id;
                item.rollback = item.file.target + ".rollback-" + id;
                QFile input(item.file.binary), old(item.file.target);
                require(input.open(QIODevice::ReadOnly) && old.open(QIODevice::ReadOnly), "Update or original could not be read");
                require(regularFile(old), "Installed target is not a regular file");
                copySynced(old, backups + "/" + stamp + "-" + QFileInfo(item.file.target).fileName(), 0755);
                copySynced(input, item.candidate, 0755);
                QFile copied(item.candidate);
                require(copied.open(QIODevice::ReadOnly), "Temporary file is not readable");
                verify(copied, item.file);
                require(linkFile(item.file.target, item.rollback) == 0,
                        "Local rollback link could not be created");
                syncDirectory(QFileInfo(item.file.target).absolutePath());
                entries.append(QJsonObject{{"target", item.file.target}, {"sha256", item.file.sha256}});
            }
            syncDirectory(backups);
            QJsonObject journal{{"schema", 1}, {"id", id}, {"version", p.version}, {"phase", "prepared"}, {"files", entries}};
            // Set before saving: even an fsync failure can leave a visible journal.
            journalPrepared = true;
            saveJson(journalPath, journal);
            for (auto &item : files) {
                checkMappings(p.target, scan); // Also catch instances that appeared while copying/backing up.
                const auto renamed = renameOperation ? renameOperation(item.candidate, item.file.target) : renameFile(item.candidate, item.file.target);
                require(renamed == 0, "Atomic replacement failed");
                item.swapped = true;
                syncDirectory(QFileInfo(item.file.target).absolutePath());
            }
            journal["phase"] = "committed";
            saveJson(journalPath, journal);
            committed = true;
        }
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        if (!committed && journalPrepared) {
            try {
                for (auto it = files.rbegin(); it != files.rend(); ++it) {
                    if (it->file.target == p.target && it->swapped) checkMappings(p.target, scan);
                    restore(*it, scan);
                }
                cleanupTransaction(files, journalPath);
                journalPrepared = false;
            } catch (const std::exception &rollbackError) {
                rollbackFailed = true;
                error += QStringLiteral("; rollback incomplete: ") + QString::fromUtf8(rollbackError.what());
            }
        }
    }
    // An unvalidated/interrupted journal can describe a partially installed pair.
    // Never tell the UI that originals were retained while recovery is unresolved.
    if (!committed && QFile::exists(journalPath)) rollbackFailed = true;
    if (!committed && !journalPrepared && !QFile::exists(journalPath)) {
        for (const auto &item : files) {
            if (!item.candidate.isEmpty()) QFile::remove(item.candidate);
            if (!item.rollback.isEmpty()) QFile::remove(item.rollback);
        }
    }
    try {
        saveJson(result, {{"success", committed}, {"fromVersion", p.fromVersion}, {"toVersion", p.version},
            {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"error", error},
            {"rollbackFailed", rollbackFailed}, {"helperUpdated", committed && p.helper.has_value()}});
        if (committed) {
            // A committed journal makes partially completed cleanup safe to resume.
            for (const auto &item : files) {
                if (QFile::exists(item.file.binary)) require(QFile::remove(item.file.binary), "Pending file could not be removed");
            }
            require(QFile::remove(directory + "/pending.json"), "Pending metadata could not be removed");
            syncDirectory(directory);
            cleanupTransaction(files, journalPath);
        }
    } catch (const std::exception &e) { error += QStringLiteral("; ") + QString::fromUtf8(e.what()); return false; }
    return committed;
}
}
