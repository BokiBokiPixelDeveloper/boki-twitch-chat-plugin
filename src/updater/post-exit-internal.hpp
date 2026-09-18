#pragma once
#include "updater/post-exit.hpp"
#include <QFile>
#include <QJsonObject>
#include <stdexcept>

namespace postexit::detail {
inline void require(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }
struct Lock {
    int value = -1;
    Lock() = default;
    ~Lock() { releaseLock(value); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};
void syncDirectory(const QString &path);
void saveJson(const QString &path, const QJsonObject &object);
void copySynced(QFile &input, const QString &path, int mode);
void verify(QFile &file, const PendingFile &pending);
int renameFile(const QString &from, const QString &to);
int linkFile(const QString &from, const QString &to);
bool sameFile(const QString &a, const QString &b);
bool regularFile(QFile &file);
}
