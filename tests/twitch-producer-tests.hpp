#pragma once
#include <QObject>

class TwitchProducerTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void delayedClearUsesServerTime();
    void pendingReturnIsBoundedAndInvalidated();
    void closeReleasesCallbackOutsideLock();
    void runtimeLogCanReadAttachmentStatus();
    void allEventsThroughLiveProducer();
    void handoffAndFreshReconnect();
    void missingScopesAndRejectedFollowDoNotBreakChat();
    void transientSubscriptionRetryAndStop();
    void delayedEnrichmentOrderingAndCancellation();
    void sharedRuntimeAndDetachedConsumers();
    void runtimeTokensAndQueuedDetach();
    void refreshBoundedAndFormEncoding();
    void nativeAdapterModerationAndLifetime();
    void shutdownFromAnotherThread();
    void enrichmentPressureAndDeadline();
    void gifUsesTheSameDispatcherEvent();
    void changedSettingsReuseTheRuntime();
    void slowSourceResubscribesWithoutASecondConnection();
};
