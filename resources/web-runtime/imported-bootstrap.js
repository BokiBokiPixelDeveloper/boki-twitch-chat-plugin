(() => {
  'use strict';
  // Installed before any third-party script. Diagnostics contain categories only,
  // never exception text, resource URLs, viewer data, or the bridge capability.
  const streamElements = document.currentScript.dataset.compatibility === 'streamelements';
  let resolveReady, rejectReady, finished = false;
  const ready = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
  ready.catch(() => {});
  const fail = reason => { if (!finished) { finished = true; rejectReady(reason); } };
  const onError = event => {
    const tag = event.target?.tagName;
    if (tag === 'SCRIPT') fail('script-resource');
    else if (tag === 'LINK' && event.target.rel === 'stylesheet') fail('stylesheet-resource');
    else if (event.target === window) fail('javascript');
  };
  const onRejection = () => fail('javascript');
  window.addEventListener('error', onError, true);
  window.addEventListener('unhandledrejection', onRejection);
  const documentReady = new Promise(resolve => document.addEventListener('DOMContentLoaded', resolve, {once:true}));
  const complete = () => setTimeout(() => {
    // Let rejected async onWidgetLoad handlers report their failure first.
    if (finished) return;
    finished = true;
    window.removeEventListener('error', onError, true);
    window.removeEventListener('unhandledrejection', onRejection);
    resolveReady();
  }, 0);
  window.BokiWidgetLifecycle = Object.freeze({ready, documentReady, complete, fail});
  if (!streamElements) documentReady.then(complete);
})();
