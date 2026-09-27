(() => {
  'use strict';
  const listeners = new Set();
  const statusListeners = new Set();
  let resolveReady;
  const ready = new Promise(resolve => { resolveReady = resolve; });
  const notify = (set, value) => { for (const callback of [...set]) { try { callback(value); } catch (error) { console.error('[WebWidget] listener failed', error); } } };
  window.BokiChat = Object.freeze({
    ready,
    onEvent(callback) { if (typeof callback !== 'function') throw new TypeError('callback must be a function'); listeners.add(callback); return () => listeners.delete(callback); },
    onStatus(callback) { if (typeof callback !== 'function') throw new TypeError('callback must be a function'); statusListeners.add(callback); return () => statusListeners.delete(callback); }
  });
  fetch('bootstrap.json', {cache:'no-store'}).then(response => response.json()).then(config => {
    const socket = new WebSocket(config.webSocketUrl);
    socket.addEventListener('open', () => socket.send(JSON.stringify({type:'hello', capability:config.capability, supportedSchemaVersions:[1]})));
    socket.addEventListener('message', message => {
      let frame; try { frame = JSON.parse(message.data); } catch { return; }
      if (frame.type === 'welcome' && frame.schemaVersion === 1) { resolveReady(Object.freeze(frame)); socket.send(JSON.stringify({type:'ready'})); notify(statusListeners, 'ready'); }
      else if (frame.type === 'events' && Array.isArray(frame.events)) { for (const event of frame.events) notify(listeners, deepFreeze(event)); socket.send(JSON.stringify({type:'ack', deliveryId:frame.deliveryId})); }
      else if (frame.type === 'reset') notify(statusListeners, 'reset');
    });
    socket.addEventListener('close', () => notify(statusListeners, 'disconnected'));
  }).catch(() => notify(statusListeners, 'unavailable'));
  function deepFreeze(value) { if (value && typeof value === 'object') { Object.freeze(value); for (const child of Object.values(value)) deepFreeze(child); } return value; }
})();
