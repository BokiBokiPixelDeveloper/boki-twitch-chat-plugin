(() => {
  'use strict';

  const config = Object.freeze(window.BokiStreamElementsConfig || {});
  const log = (message) => console.warn(`[StreamElements][Package:${config.packageId || 'builtin'}][Instance:${config.instanceId || 'unknown'}] ${message}`);
  const maxMessages = Math.max(1, Math.min(200, Number(config.chatLayout?.maxMessages) || 20));
  const maxHeight = Math.max(64, Math.min(4320, Number(config.chatLayout?.maxHeight) || 600));
  const fieldValues = {...config.fieldData};
  // These are Scrapbook's existing field names. Keep the imported files intact.
  if ('alignMessages' in fieldValues) fieldValues.alignMessages = 'bottom';
  if ('msgLimit' in fieldValues) fieldValues.msgLimit = false;
  const fields = Object.freeze(fieldValues);
  let maintainLayout = () => {};
  const installChatLayout = () => {
    const container = document.querySelector('.main-container');
    if (!container) return;
    const style = document.createElement('style');
    style.textContent = `html, body {height:100%; margin:0; overflow:hidden;}
      .main-container {box-sizing:border-box!important; height:min(${maxHeight}px, 100vh)!important;
        max-height:100vh!important; overflow-x:hidden!important; overflow-y:auto!important;
        display:flex!important; flex-direction:column!important; justify-content:flex-start!important;
        scrollbar-width:none; overflow-anchor:none;}
      .main-container::-webkit-scrollbar {display:none;}
      .main-container > .message-row, .main-container > .alert-row {flex-shrink:0!important;}
      .main-container > :first-child {margin-top:auto!important;}`;
    document.head.appendChild(style);
    const observed = new Set();
    const resize = new ResizeObserver(() => maintainLayout());
    maintainLayout = () => {
      const rows = [...container.children].filter(row => row.matches('.message-row,.alert-row'));
      for (const row of rows.slice(0, Math.max(0, rows.length - maxMessages))) row.remove();
      for (const row of observed) {
        if (!row.isConnected) { resize.unobserve(row); observed.delete(row); }
      }
      for (const row of rows) {
        if (row.isConnected && !observed.has(row)) { observed.add(row); resize.observe(row); }
      }
      container.scrollTop = container.scrollHeight;
    };
    new MutationObserver(maintainLayout).observe(container, {childList:true, subtree:true, characterData:true});
    resize.observe(container);
    container.addEventListener('load', maintainLayout, true);
    document.fonts?.ready.then(maintainLayout);
    maintainLayout();
  };
  const visibleMessages = new Map();
  // StreamElements widgets commonly interpolate identity and alert strings into
  // HTML templates. Keep the core DTO semantic and encode only those adapter
  // presentation fields. Chat text and emote names stay semantic because the
  // StreamElements message rendering contract (and Scrapbook) encodes them.
  const htmlText = value => String(value ?? '').replace(/[&<>"']/g, character => ({
    '&': '&#38;', '<': '&#60;', '>': '&#62;', '"': '&#34;', "'": '&#39;'
  })[character]);
  const user = value => ({
    id: String(value?.id || ''), login: htmlText(value?.login), displayName: htmlText(value?.displayName),
    color: value?.color ?? null
  });
  const safeId = (prefix, value) => `${prefix}-${String(value || '').replace(/[^A-Za-z0-9_-]/g, character => `_${character.codePointAt(0).toString(16)}_`)}`;
  const message = (data, header) => {
    const viewer = user(data.user);
    const badges = Array.isArray(data.badges) ? data.badges.map(badge => ({
      type: String(badge.type || ''), version: String(badge.version || ''), url: badge.imageUrl || undefined,
      description: htmlText(badge.info)
    })) : [];
    const emotes = (data.fragments || []).filter(fragment => fragment.type === 'emote' && fragment.imageUrl).map(fragment => ({
      type: String(fragment.provider || 'twitch'), name: String(fragment.text || ''), id: String(fragment.emoteId || ''),
      gif: Boolean(fragment.twitch?.supportsAnimated), urls: {'1': fragment.imageUrl, '2': fragment.imageUrl, '4': fragment.imageUrl},
      start: Number.isInteger(fragment.sourceRange?.offset) ? fragment.sourceRange.offset : undefined,
      end: Number.isInteger(fragment.sourceRange?.offset) && Number.isInteger(fragment.sourceRange?.length)
        ? fragment.sourceRange.offset + fragment.sourceRange.length - 1 : undefined,
      zeroWidth: Boolean(fragment.zeroWidth)
    }));
    const badgeTags = badges.map(badge => `${badge.type}/${badge.version}`).join(',');
    const role = type => badges.some(badge => badge.type === type) ? '1' : '0';
    return {time: Date.parse(header.timestamp) || Date.now(), tags: {badges: badgeTags, color: viewer.color,
      'display-name': viewer.displayName, 'user-id': viewer.id, id: data.messageId, mod: role('moderator'),
      subscriber: role('subscriber'), 'user-type': role('moderator') === '1' ? 'mod' : ''}, nick: viewer.login,
      userId: safeId('user', viewer.id), displayName: viewer.displayName, displayColor: viewer.color, badges,
      channel: config.channel?.username || '', text: String(data.text || ''), isAction: data.metadata?.messageType === 'action',
      emotes, msgId: safeId('message', data.messageId)};
  };
  const event = (listener, value) => window.dispatchEvent(new CustomEvent('onEventReceived', {detail: {listener, event: value}}));
  const displayName = value => user(value).displayName || 'Anonymous';
  const notice = value => value?.notice || {};
  const subscription = (data, kind) => {
    const text = htmlText(notice(data).text);
    if (kind === 'CommunityGiftSubscription') return {name: displayName(data.gifter?.user), sender: displayName(data.gifter?.user), amount: data.count,
      message: text, gifted: true, bulkGifted: true, isCommunityGift: false, tier: data.tier};
    if (kind === 'GiftSubscription') return {name: displayName(data.recipient), sender: displayName(data.gifter?.user), amount: 1,
      message: text, gifted: true, bulkGifted: false, isCommunityGift: Boolean(data.communityGiftId), tier: data.terms?.tier};
    return {name: displayName(notice(data).user), amount: kind === 'Resubscription' ? data.cumulativeMonths : 1, message: text,
      gifted: Boolean(data.isGift), bulkGifted: false, isCommunityGift: false, tier: data.terms?.tier,
      streak: data.streakMonths ?? null, duration: data.terms?.durationMonths ?? null, isPrime: data.terms?.isPrime ?? null};
  };
  const dispatch = generic => {
    const data = generic.data || {};
    switch (generic.type) {
    case 'ChatMessage': {
      const mapped = message(data, generic.header || {});
      visibleMessages.set(mapped.msgId, mapped.userId);
      event('message', {data: mapped});
      while (visibleMessages.size > maxMessages) {
        const oldest = visibleMessages.keys().next().value;
        visibleMessages.delete(oldest);
        event('delete-message', {msgId: oldest});
      }
      break;
    }
    case 'MessageDeleted': {
      const msgId = safeId('message', data.messageId);
      visibleMessages.delete(msgId);
      event('delete-message', {msgId});
      break;
    }
    case 'ChatCleared': {
      const target = data.user ? safeId('user', data.user.id) : null;
      for (const [msgId, userId] of [...visibleMessages]) {
        if (target && userId !== target) continue;
        event('delete-message', {msgId});
        visibleMessages.delete(msgId);
      }
      if (target) event('delete-messages', {userId: target});
      break;
    }
    case 'Follow': event('follower-latest', {name: displayName(data.user)}); break;
    case 'Subscription': case 'Resubscription': case 'GiftSubscription': case 'CommunityGiftSubscription':
      event('subscriber-latest', subscription(data, generic.type)); break;
    case 'Cheer': event('cheer-latest', {name: displayName(data.user), amount: data.bits, message: htmlText(data.text)}); break;
    case 'Raid': event('raid-latest', {name: displayName(data.from), amount: data.viewers, viewers: data.viewers}); break;
    default: log(`Unsupported generic event: ${generic.type}`);
    }
    maintainLayout();
  };
  const unsupported = path => (...args) => {
    log(`Unsupported API call: SE_API.${path}`);
    if (path.endsWith('.get')) return Promise.reject(new Error(`SE_API.${path} is unavailable`));
    return undefined;
  };
  const api = new Proxy({}, {get(_, property) {
    if (property === 'then') return undefined;
    if (typeof property === 'symbol') return undefined;
    if (property === 'store') return Object.freeze({get: unsupported('store.get'), set: unsupported('store.set')});
    if (property === 'counters') return Object.freeze({get: unsupported('counters.get')});
    if (property === 'sanitize') return ({message: value} = {}) => htmlText(value);
    if (property === 'cheerFilter') return value => htmlText(value);
    if (property === 'getOverlayStatus') return () => Promise.resolve({isEditorMode: false, isLive: true});
    if (property === 'resumeQueue') return () => undefined;
    if (property === 'setField') return unsupported('setField');
    return unsupported(String(property));
  }});
  Object.defineProperty(window, 'SE_API', {value: api, configurable: false, writable: false});
  log('Adapter started');
  BokiChat.onEvent(dispatch);
  Promise.all([BokiChat.ready, window.BokiWidgetLifecycle?.documentReady]).then(() => {
    log('onWidgetLoad dispatch started');
    window.dispatchEvent(new CustomEvent('onWidgetLoad', {detail: {fieldData: fields, channel: {username: config.channel?.username || ''},
      currency: {symbol: config.currency?.symbol || ''}, session: {data: {}}, recents: []}}));
    installChatLayout();
    log('onWidgetLoad dispatched');
    window.BokiWidgetLifecycle?.complete();
  });
})();
