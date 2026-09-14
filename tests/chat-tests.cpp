#include "chat-tests.hpp"
#include "chat/emote-service.hpp"
#include "renderer/message-layout.hpp"

#include <QBuffer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPainter>
#include <QTest>
#include <cstring>

namespace {
QByteArray fixture(const char *name)
{
    QFile file(QStringLiteral(FIXTURE_DIR) + QLatin1Char('/') + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Missing fixture: %s", name);
    return file.readAll();
}

QByteArray png()
{
    QImage image(24, 12, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

ChatFragment emote(const QString &name, const QString &url)
{
    ChatFragment out{ChatFragment::Type::Emote, name};
    out.imageUrl = QUrl(url);
    return out;
}

ChatMessage imageMessage(const QString &name, const QString &url)
{
    ChatMessage out{name, QStringLiteral("emote")};
    out.fragments = {emote(out.text, url)};
    return out;
}

struct Response {
    QByteArray bytes;
    int status = 200;
    int delay = 0;
};

class FakeReply : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest &request, Response response, QObject *parent)
        : QNetworkReply(parent), bytes_(std::move(response.bytes))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(response.delay, this, [this, status = response.status]() {
            if (isFinished())
                return;
            if (status != 200)
                setError(ContentNotFoundError, QStringLiteral("Missing test image"));
            ready_ = true;
            Q_EMIT readyRead();
            if (!isFinished()) {
                setFinished(true);
                Q_EMIT finished();
            }
        });
    }
    void abort() override
    {
        if (isFinished())
            return;
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        Q_EMIT finished();
    }
    qint64 bytesAvailable() const override { return (ready_ ? bytes_.size() - offset_ : 0) + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = std::min(maxSize, ready_ ? bytes_.size() - offset_ : qint64(0));
        if (!count)
            return -1;
        std::memcpy(data, bytes_.constData() + offset_, static_cast<size_t>(count));
        offset_ += count;
        return count;
    }
private:
    QByteArray bytes_;
    qint64 offset_ = 0;
    bool ready_ = false;
};

class FakeNetwork : public QNetworkAccessManager {
public:
    QHash<QString, Response> responses;
    QList<QNetworkRequest> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.push_back(request);
        return new FakeReply(request, responses.value(request.url().toString(), {{}, 404, 0}), this);
    }
};
} // namespace

void ChatTests::initTestCase()
{
    QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral(":/bokis-twitch-chat-plugin/fonts/NotoColorEmoji.ttf")) >= 0);
}

void ChatTests::twitchFragments()
{
    const auto event = QJsonDocument::fromJson(R"({"chatter_user_name":"Boki","color":"#123456","message":{
      "text":"Hi 👀 Kappa Dance @friend","fragments":[
      {"type":"text","text":"Hi 👀 "},
      {"type":"emote","text":"Kappa","emote":{"id":"25","format":["static"]}},
      {"type":"text","text":" "},
      {"type":"emote","text":"Dance","emote":{"id":"emotesv2_abc","format":["static","animated"]}},
      {"type":"mention","text":" @friend"}]}})").object();
    const auto message = parseTwitchMessage(event);
    QCOMPARE(message.fragments.size(), size_t(5));
    QCOMPARE(message.userColor, QColor(QStringLiteral("#123456")));
    QCOMPARE(message.fragments[0].text, QStringLiteral("Hi 👀 "));
    QCOMPARE(message.fragments[1].imageUrl.path(), QStringLiteral("/emoticons/v2/25/static/dark/3.0"));
    QVERIFY(message.fragments[3].imageUrl.path().contains(QStringLiteral("/animated/")));
    QVERIFY(message.fragments[3].fallbackUrl.path().contains(QStringLiteral("/static/")));
    QCOMPARE(message.fragments[4].text, QStringLiteral(" @friend"));
}

void ChatTests::malformedFragmentsKeepText()
{
    const auto event = QJsonDocument::fromJson(R"({"message":{"text":"full 👨‍👩‍👧‍👦 text","fragments":[{"text":"partial"}]}})").object();
    auto message = parseTwitchMessage(event);
    QCOMPARE(message.fragments.size(), size_t(1));
    QCOMPARE(message.fragments[0].text, message.text);
    const auto invalid = QJsonDocument::fromJson(R"({"message":{"text":"bad","fragments":[{"type":"emote","text":"bad","emote":{"id":"../../bad"}}]}})").object();
    QVERIFY(parseTwitchMessage(invalid).fragments[0].imageUrl.isEmpty());
}

void ChatTests::providerCatalogs()
{
    auto bttv = parseEmoteCatalog(EmoteProvider::BetterTTV, QJsonDocument::fromJson(R"({
      "channelEmotes":[{"id":"123","code":"Dance","animated":true,"modifier":true}],
      "sharedEmotes":[{"id":"456","code":"Shared"}]})"));
    QCOMPARE(bttv.size(), 2);
    QVERIFY(bttv.value(QStringLiteral("Dance")).zeroWidth);
    QCOMPARE(bttv.value(QStringLiteral("Shared")).imageUrl.toString(), QStringLiteral("https://cdn.betterttv.net/emote/456/3x"));
    auto ffz = parseEmoteCatalog(EmoteProvider::FrankerFaceZ, QJsonDocument::fromJson(R"({
      "default_sets":[3],"sets":{"3":{"emoticons":[{"id":42,"name":"Hello","urls":{"1":"//cdn.frankerfacez.com/1","4":"//cdn.frankerfacez.com/4"},"animated":{"4":"//cdn.frankerfacez.com/animated"}}]},
      "9":{"emoticons":[{"id":9,"name":"NotGlobal","urls":{"1":"//cdn.frankerfacez.com/9"}}]}}})"));
    QCOMPARE(ffz.size(), 1);
    QCOMPARE(ffz.value(QStringLiteral("Hello")).imageUrl.toString(), QStringLiteral("https://cdn.frankerfacez.com/animated"));
    QCOMPARE(ffz.value(QStringLiteral("Hello")).fallbackUrl.toString(), QStringLiteral("https://cdn.frankerfacez.com/4"));
    auto seven = parseEmoteCatalog(EmoteProvider::SevenTV, QJsonDocument::fromJson(R"({"emote_set":{"emotes":[
      {"id":"abc","name":"Alias","data":{"name":"Original","flags":256,"host":{"url":"//cdn.7tv.app/emote/abc","files":[
      {"name":"4x.avif","format":"AVIF","height":128},
      {"name":"1x.webp","format":"WEBP","height":32},
      {"name":"3x.webp","static_name":"3x_static.webp","format":"WEBP","height":96}]}}}]}})"));
    QCOMPARE(seven.size(), 1);
    QVERIFY(seven.value(QStringLiteral("Alias")).zeroWidth);
    QCOMPARE(seven.value(QStringLiteral("Alias")).imageUrl.fileName(), QStringLiteral("3x.webp"));
    QCOMPARE(seven.value(QStringLiteral("Alias")).fallbackUrl.fileName(), QStringLiteral("3x_static.webp"));
}

void ChatTests::wholeTokensAndPrecedence()
{
    EmoteCatalog catalog;
    const QString token = QStringLiteral("Dance");
    catalog.replace(EmoteProvider::SevenTV, false, {{token, emote(token, QStringLiteral("https://7tv.io/global"))}});
    catalog.replace(EmoteProvider::BetterTTV, true, {{token, emote(token, QStringLiteral("https://betterttv.com/channel"))}});
    ChatMessage message{QStringLiteral("User"), QStringLiteral("Dance Dance! xDance\tDance")};
    catalog.apply(message);
    QCOMPARE(message.fragments.size(), size_t(3));
    QCOMPARE(message.fragments[0].imageUrl.host(), QStringLiteral("betterttv.com"));
    QCOMPARE(message.fragments[1].text, QStringLiteral(" Dance! xDance\t"));
    catalog.replace(EmoteProvider::SevenTV, true, {{token, emote(token, QStringLiteral("https://7tv.io/channel"))}});
    ChatMessage native = imageMessage(QStringLiteral("User"), QStringLiteral("https://static-cdn.jtvnw.net/native"));
    native.fragments[0].text = token;
    catalog.apply(native);
    QCOMPARE(native.fragments[0].imageUrl.host(), QStringLiteral("static-cdn.jtvnw.net"));
    ChatMessage next{QStringLiteral("User"), token};
    catalog.apply(next);
    QCOMPARE(next.fragments[0].imageUrl.host(), QStringLiteral("7tv.io"));
    catalog.clear();
    next.fragments.clear();
    catalog.apply(next);
    QCOMPARE(next.fragments[0].type, ChatFragment::Type::Text);
}

void ChatTests::imageDecoding()
{
    const auto still = decodeChatImage(png());
    QCOMPARE(still.frames.size(), size_t(1));
    QCOMPARE(still.frames[0].size(), QSize(24, 12));
    for (const char *name : {"animation.gif", "animation.webp"}) {
        const auto image = decodeChatImage(fixture(name));
        QCOMPARE(image.frames.size(), size_t(3));
        QCOMPARE(image.delaysMs, std::vector<int>({40, 120, 80}));
        QCOMPARE(image.frames[0].size(), QSize(16, 12));
        QVERIFY(image.frames[0] != image.frames[1]);
        QCOMPARE(image.frames[0].pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.frames[1].pixelColor(0, 0).alpha(), 0);
    }
    QVERIFY(decodeChatImage("not an image").frames.empty());
    QVERIFY(decodeChatImage(QByteArray(8 * 1024 * 1024 + 1, 'x')).frames.empty());
    QImage huge(2049, 1, QImage::Format_RGBA8888);
    huge.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    huge.save(&buffer, "PNG");
    QVERIFY(decodeChatImage(bytes).frames.empty());
}

void ChatTests::longAnimationFitsMemoryBudget()
{
    const QByteArray original = fixture("animation.gif");
    const qsizetype firstFrame = original.indexOf(QByteArray::fromHex("21f904"));
    QVERIFY(firstFrame > 0);
    QByteArray bytes = original.left(firstFrame);
    // A 256x256 logical canvas with 492 frames exceeds 16 MiB at full resolution.
    bytes[6] = 0; bytes[7] = 1;
    bytes[8] = 0; bytes[9] = 1;
    const QByteArray frames = original.mid(firstFrame, original.size() - firstFrame - 1);
    for (int i = 0; i < 164; ++i)
        bytes.append(frames);
    bytes.append(';');
    const auto image = decodeChatImage(bytes);
    QCOMPARE(image.frames.size(), size_t(492));
    QVERIFY(image.frames[0].width() < 256);
    qint64 memory = 0;
    for (const auto &frame : image.frames)
        memory += frame.sizeInBytes();
    QVERIFY(memory <= 16 * 1024 * 1024);
    QCOMPARE(image.delaysMs.back(), 80);
}

void ChatTests::animationUsesPerFrameDelays()
{
    const auto image = decodeChatImage(fixture("animation.gif"));
    int frame = 0;
    double elapsed = 0;
    QVERIFY(advanceAnimation(image, frame, elapsed, 40));
    QCOMPARE(frame, 1);
    QVERIFY(!advanceAnimation(image, frame, elapsed, 119));
    QCOMPARE(frame, 1);
    QVERIFY(advanceAnimation(image, frame, elapsed, 1));
    QCOMPARE(frame, 2);
    QVERIFY(advanceAnimation(image, frame, elapsed, 80));
    QCOMPARE(frame, 0);
    advanceAnimation(image, frame, elapsed, 240000 + 165);
    QCOMPARE(frame, 2);
    QCOMPARE(elapsed, 5.0);
    frame = 0; elapsed = 0;
    for (int i = 0; i < 60; ++i)
        advanceAnimation(image, frame, elapsed, 1000.0 / 60.0);
    // 1000 ms = four 240 ms loops plus 40 ms; no per-tick rounding drift.
    QVERIFY(std::abs(elapsed) < 0.001 || std::abs(elapsed - 40) < 0.001);
    QVERIFY(!advanceAnimation(image, frame, elapsed, -10));
}

void ChatTests::coloredUnicode_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("eyes") << QStringLiteral("👀");
    QTest::newRow("heart-selector") << QStringLiteral("❤️");
    QTest::newRow("skin-tone") << QStringLiteral("👍🏽");
    QTest::newRow("family-zwj") << QStringLiteral("👨‍👩‍👧‍👦");
    QTest::newRow("profession-zwj") << QStringLiteral("👩🏽‍💻");
    QTest::newRow("flag") << QStringLiteral("🇩🇪");
    QTest::newRow("keycap") << QStringLiteral("1️⃣");
}

void ChatTests::coloredUnicode()
{
    QFETCH(QString, text);
    const auto layout = layoutMessage({{}, text, Qt::white}, QStringLiteral("Liberation Sans"), 500, 48, 2.5);
    int coloredPixels = 0;
    int visiblePixels = 0;
    for (int y = 0; y < layout.text.height(); ++y) {
        for (int x = 0; x < layout.text.width(); ++x) {
            const QColor pixel = layout.text.pixelColor(x, y);
            if (pixel.alpha() > 100)
                ++visiblePixels;
            if (pixel.alpha() > 100 && std::max({pixel.red(), pixel.green(), pixel.blue()}) -
                std::min({pixel.red(), pixel.green(), pixel.blue()}) > 30)
                ++coloredPixels;
        }
    }
    // Noto intentionally draws eyes and family pictograms mostly in grayscale.
    if (text != QStringLiteral("👀") && text != QStringLiteral("👨‍👩‍👧‍👦"))
        QVERIFY2(coloredPixels > 20, qPrintable(QStringLiteral("No color pixels for %1").arg(text)));
    QVERIFY(visiblePixels > 400);
    QVERIFY(layout.text.width() < 160); // sequence must be shaped as one glyph, not split
}

void ChatTests::inlineLayoutAndOverlay()
{
    ChatMessage message{QStringLiteral("Boki"), QStringLiteral("Hi ❤️ wide overlay done")};
    auto base = emote(QStringLiteral("wide"), {});
    base.image = std::make_shared<const DecodedImage>(decodeChatImage(png()));
    auto overlay = base;
    overlay.zeroWidth = true;
    message.fragments = {{ChatFragment::Type::Text, QStringLiteral("Hi ❤️ ")}, base,
        {ChatFragment::Type::Text, QStringLiteral(" ")}, overlay,
        {ChatFragment::Type::Text, QStringLiteral(" done")}};
    auto layout = layoutMessage(message, {}, 500, 48, 2.5);
    QCOMPARE(layout.emotes.size(), size_t(2));
    QCOMPARE(layout.emotes[0].rect.width() / layout.emotes[0].rect.height(), 2.0);
    QCOMPARE(layout.emotes[0].rect.center(), layout.emotes[1].rect.center());
    QVERIFY(layout.emotes[0].rect.x() > 50);
    QVERIFY(QRectF(layout.text.rect()).contains(layout.emotes[0].rect));
    message.fragments.erase(message.fragments.begin() + 2, message.fragments.begin() + 4);
    const auto withoutOverlay = layoutMessage(message, {}, 500, 48, 2.5);
    QCOMPARE(layout.text.size(), withoutOverlay.text.size());
    // A failed asset renders its original name instead of an empty object.
    message.fragments[1].image.reset();
    const auto fallback = layoutMessage(message, {}, 500, 48, 2.5);
    QVERIFY(fallback.emotes.empty());
    QVERIFY(fallback.text != withoutOverlay.text);
}

void ChatTests::cacheDeduplicatesAndRejectsFailures()
{
    FakeNetwork network;
    const QUrl url(QStringLiteral("https://images.example/emote"));
    network.responses.insert(url.toString(), {png(), 200, 20});
    ImageCache cache(&network);
    int completed = 0;
    ImageAsset first;
    cache.request(url, [&](ImageAsset image) { first = image; ++completed; });
    cache.request(url, [&](ImageAsset image) { QVERIFY(image); ++completed; });
    QCOMPARE(network.requests.size(), 1);
    QTRY_COMPARE(completed, 2);
    cache.request(url, [&](ImageAsset image) { QCOMPARE(image, first); ++completed; });
    QCOMPARE(completed, 3);
    QCOMPARE(network.requests.size(), 1);
    const QUrl bad(QStringLiteral("https://images.example/missing"));
    cache.request(bad, [&](ImageAsset image) { QVERIFY(!image); ++completed; });
    QTRY_COMPARE(completed, 4);
    cache.request(bad, [&](ImageAsset image) { QVERIFY(!image); ++completed; });
    QCOMPARE(completed, 5);
    QCOMPARE(network.requests.size(), 2);
    cache.request(QUrl(QStringLiteral("file:///etc/passwd")), [&](ImageAsset image) { QVERIFY(!image); ++completed; });
    QCOMPARE(network.requests.size(), 2);
    for (const auto &request : network.requests) {
        QVERIFY(!request.hasRawHeader("Authorization"));
        QVERIFY(!request.hasRawHeader("Client-Id"));
    }
}

void ChatTests::orderedMessagesAndStaticFallback()
{
    FakeNetwork network;
    network.responses.insert(QStringLiteral("https://images.example/slow"), {png(), 200, 80});
    network.responses.insert(QStringLiteral("https://images.example/static"), {png(), 200, 0});
    std::vector<ChatMessage> delivered;
    EmoteService service([&](ChatMessage message) { delivered.push_back(std::move(message)); }, &network);
    service.resolve(imageMessage(QStringLiteral("first"), QStringLiteral("https://images.example/slow")));
    auto second = imageMessage(QStringLiteral("second"), QStringLiteral("https://images.example/missing"));
    second.fragments[0].fallbackUrl = QUrl(QStringLiteral("https://images.example/static"));
    service.resolve(std::move(second));
    service.resolve({QStringLiteral("third"), QStringLiteral("plain")});
    QTRY_COMPARE(delivered.size(), size_t(3));
    QCOMPARE(delivered[0].userName, QStringLiteral("first"));
    QCOMPARE(delivered[1].userName, QStringLiteral("second"));
    QCOMPARE(delivered[2].userName, QStringLiteral("third"));
    QVERIFY(delivered[0].fragments[0].image);
    QVERIFY(delivered[1].fragments[0].image);
}

void ChatTests::timeoutAndDestruction()
{
    FakeNetwork network;
    network.responses.insert(QStringLiteral("https://images.example/stall"), {png(), 200, 10000});
    int delivered = 0;
    {
        EmoteService service([&](ChatMessage message) { QVERIFY(!message.fragments[0].image); ++delivered; }, &network);
        service.resolve(imageMessage(QStringLiteral("first"), QStringLiteral("https://images.example/stall")));
        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 3500);
        service.resolve(imageMessage(QStringLiteral("second"), QStringLiteral("https://images.example/stall")));
        service.clear();
    }
    const auto replies = network.findChildren<QNetworkReply *>();
    for (auto *reply : replies)
        reply->abort();
    QCOMPARE(delivered, 1);
}

void ChatTests::providerStartupAndChannelChange()
{
    FakeNetwork network;
    network.responses.insert(QStringLiteral("https://api.betterttv.net/3/cached/users/twitch/1"), {
        R"({"channelEmotes":[{"id":"123","code":"Dance"}]})", 200, 20});
    network.responses.insert(QStringLiteral("https://cdn.betterttv.net/emote/123/3x"), {png(), 200, 0});
    std::vector<ChatMessage> delivered;
    EmoteService service([&](ChatMessage message) { delivered.push_back(std::move(message)); }, &network);
    service.setChannel(QStringLiteral("1"));
    service.resolve({QStringLiteral("one"), QStringLiteral("Dance")});
    QTRY_COMPARE(delivered.size(), size_t(1));
    QVERIFY(delivered[0].fragments[0].image);
    service.setChannel(QStringLiteral("2"));
    service.resolve({QStringLiteral("two"), QStringLiteral("Dance")});
    QTRY_COMPARE(delivered.size(), size_t(2));
    QVERIFY(!delivered[1].fragments[0].image);
    QCOMPARE(delivered[1].fragments[0].type, ChatFragment::Type::Text);
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    ChatTests tests;
    return QTest::qExec(&tests, argc, argv);
}
