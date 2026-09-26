#pragma once

#include <QObject>

class ChatTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void twitchFragments();
    void malformedFragmentsKeepText();
    void providerCatalogs();
    void wholeTokensAndPrecedence();
    void structuredProviderIdentityAndRanges();
    void mentionsAndCheermotesKeepTheirMetadata();
    void legacyMediaAndMalformedRanges();
    void imageDecoding();
    void longAnimationFitsMemoryBudget();
    void animationUsesPerFrameDelays();
    void coloredUnicode_data();
    void coloredUnicode();
    void inlineLayoutAndOverlay();
    void cacheDeduplicatesAndRejectsFailures();
    void orderedMessagesAndStaticFallback();
    void timeoutAndDestruction();
    void providerStartupAndChannelChange();
};
