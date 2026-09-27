#include "web/widget-resource-request.hpp"
#include <QList>

std::optional<QByteArray> WidgetResourceRequest::resourceName(
    const QByteArray &request, const QByteArray &capability, quint16 port)
{
    const int headerEnd = request.indexOf("\r\n\r\n");
    if (headerEnd < 0 || headerEnd + 4 != request.size()) return std::nullopt;
    const QList<QByteArray> lines = request.left(headerEnd).split('\n');
    if (lines.isEmpty()) return std::nullopt;

    const QList<QByteArray> requestParts = lines.first().trimmed().split(' ');
    if (requestParts.size() != 3 || requestParts.at(0) != "GET" || requestParts.at(2) != "HTTP/1.1")
        return std::nullopt;
    const QByteArray prefix = "/" + capability + "/";
    const QByteArray target = requestParts.at(1);
    if (!target.startsWith(prefix)) return std::nullopt;
    const QByteArray name = target.mid(prefix.size());
    if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains('%') || name.contains('?') || name.contains('#'))
        return std::nullopt;

    const QByteArray expectedHost = "127.0.0.1:" + QByteArray::number(port);
    int hostCount = 0;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0) return std::nullopt;
        if (line.left(colon).compare("host", Qt::CaseInsensitive) == 0) {
            ++hostCount;
            if (line.mid(colon + 1).trimmed() != expectedHost) return std::nullopt;
        }
    }
    if (hostCount != 1) return std::nullopt;
    return name;
}
