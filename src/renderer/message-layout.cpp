#include "renderer/message-layout.hpp"

#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QPainter>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextObjectInterface>
#include <algorithm>
#include <cmath>

namespace {
constexpr int emoteObject = QTextFormat::UserObject + 1;
constexpr int emoteIndex = QTextFormat::UserProperty + 1;

// QTextDocument handles shaping, bidi and color glyphs. The inline object handler
// reserves space and records positions; OBS draws emote textures separately.
class EmoteObject : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    explicit EmoteObject(std::vector<InlineEmote> &emotes) : emotes_(emotes) {}

    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override
    {
        const auto &emote = emotes_.at(static_cast<size_t>(format.intProperty(emoteIndex)));
        return {emote.overlayBase >= 0 ? 0 : emote.rect.width() + 4, emote.rect.height()};
    }

    void drawObject(QPainter *, const QRectF &rect, QTextDocument *, int, const QTextFormat &format) override
    {
        auto &emote = emotes_.at(static_cast<size_t>(format.intProperty(emoteIndex)));
        emote.rect.moveTopLeft(rect.topLeft() + QPointF(2, 0));
    }

private:
    std::vector<InlineEmote> &emotes_;
};
} // namespace

MessageLayout layoutMessage(const ChatMessage &message, const QString &fontFamily,
                            int fontWeight, int fontPx, float outlineWidth)
{
    MessageLayout out;
    EmoteObject handler(out.emotes);
    QTextDocument document;
    document.documentLayout()->registerHandler(emoteObject, &handler);
    QFont font;
    if (!fontFamily.isEmpty())
        font.setFamily(fontFamily);
    auto families = font.families();
    families.append(QStringLiteral("Noto Color Emoji"));
    font.setFamilies(families);
    font.setPixelSize(fontPx);
    font.setWeight(static_cast<QFont::Weight>(fontWeight));
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferFullHinting);
    document.setDefaultFont(font);
    document.setDocumentMargin(std::max({6.0, fontPx / 5.0, static_cast<double>(outlineWidth) + 2.0}));
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    QTextCursor cursor(&document);
    QTextCharFormat format;
    format.setForeground(message.userColor.isValid() ? message.userColor : QColor(0x91, 0xC8, 0xFF));
    format.setFontWeight(std::max(fontWeight, 600));
    cursor.insertText(message.userName + QStringLiteral(": "), format);
    format.setFontWeight(fontWeight);
    format.setForeground(Qt::white);
    auto fragments = message.fragments;
    if (fragments.empty())
        fragments = {{ChatFragment::Type::Text, message.text}};
    int lastBase = -1;
    for (size_t i = 0; i < fragments.size(); ++i) {
        const auto &fragment = fragments[i];
        if (fragment.image && !fragment.image->frames.empty()) {
            const QSize size = fragment.image->frames.front().size();
            const qreal height = fontPx * 1.25;
            const qreal width = height * size.width() / std::max(1, size.height());
            InlineEmote emote{fragment.image, QRectF(0, 0, width, height), fragment.zeroWidth ? lastBase : -1};
            const int index = static_cast<int>(out.emotes.size());
            out.emotes.push_back(std::move(emote));
            QTextCharFormat objectFormat;
            objectFormat.setObjectType(emoteObject);
            objectFormat.setProperty(emoteIndex, index);
            objectFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            cursor.insertText(QString(QChar::ObjectReplacementCharacter), objectFormat);
            if (!fragment.zeroWidth || lastBase < 0)
                lastBase = index;
        } else {
            // Provider overlay syntax separates the overlay token with whitespace.
            // It must not introduce a visible gap between the base and its overlay.
            const bool overlaySeparator = fragment.text.trimmed().isEmpty() && lastBase >= 0 &&
                i + 1 < fragments.size() && fragments[i + 1].zeroWidth && fragments[i + 1].image;
            if (!overlaySeparator) {
                QString text = fragment.text;
                text.replace(QLatin1Char('\n'), QLatin1Char(' '));
                text.replace(QLatin1Char('\r'), QLatin1Char(' '));
                cursor.insertText(text, format);
            }
            if (!fragment.text.trimmed().isEmpty())
                lastBase = -1;
        }
    }
    const QSizeF size = document.size();
    // Bound allocations for unusually long/malformed input and GPU texture limits.
    const int width = std::clamp(static_cast<int>(std::ceil(size.width())), 1, 16384);
    const int height = std::clamp(static_cast<int>(std::ceil(size.height())), 1, 2048);
    QImage content(width, height, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::transparent);
    {
        QPainter painter(&content);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        document.drawContents(&painter);
    }
    for (auto &emote : out.emotes) {
        if (emote.overlayBase >= 0)
            emote.rect.moveCenter(out.emotes[static_cast<size_t>(emote.overlayBase)].rect.center());
    }
    // Outline the raster alpha, then paint the original colored text over it.
    // Converting glyphs to QPainterPath discards bitmap/color emoji glyphs.
    out.text = QImage(content.size(), QImage::Format_ARGB32_Premultiplied);
    out.text.fill(Qt::transparent);
    QPainter painter(&out.text);
    if (outlineWidth > 0) {
        QImage mask = content.copy();
        QPainter maskPainter(&mask);
        maskPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        maskPainter.fillRect(mask.rect(), QColor(0, 0, 0, 220));
        maskPainter.end();
        const qreal radius = outlineWidth / 2.0;
        const int samples = std::max(8, static_cast<int>(std::ceil(radius * 8)));
        for (int i = 0; i < samples; ++i) {
            const qreal angle = i * 6.283185307179586 / samples;
            painter.drawImage(QPointF(std::cos(angle) * radius, std::sin(angle) * radius), mask);
        }
    }
    painter.drawImage(0, 0, content);
    painter.end();
    out.text = out.text.convertToFormat(QImage::Format_RGBA8888);
    return out;
}

#include "message-layout.moc"
