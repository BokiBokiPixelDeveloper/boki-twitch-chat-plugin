(() => {
  'use strict';
  const chat = document.getElementById('chat');
  const events = document.getElementById('events');
  const rows = new Map();
  const eventTimers = new Set();
  const messageKey = (channelId, messageId) => `${channelId}:${messageId}`;
  const text = value => document.createTextNode(value ?? '');

  function image(url, fallback, className) {
    const element = document.createElement('img');
    element.className = className;
    element.src = url;
    element.alt = fallback;
    element.addEventListener('error', () => element.replaceWith(text(fallback)), {once: true});
    return element;
  }

  function renderMessage(data, header) {
    const row = document.createElement('div');
    row.className = 'row';
    for (const badge of data.badges) {
      if (badge.imageUrl) row.append(image(badge.imageUrl, badge.type, 'badge-image'));
      else {
        const label = document.createElement('span');
        label.className = 'badge';
        label.textContent = badge.type;
        row.append(label);
      }
    }
    const name = document.createElement('span');
    name.className = 'name';
    name.textContent = data.user.displayName;
    if (data.user.color) name.style.color = data.user.color;
    row.append(name);
    for (const fragment of data.fragments) {
      row.append(fragment.type === 'emote' && fragment.imageUrl
        ? image(fragment.imageUrl, fragment.text, 'emote') : text(fragment.text));
    }
    const key = messageKey(header.channelId, data.messageId);
    const previous = rows.get(key);
    if (previous) previous.element.remove();
    rows.set(key, {element: row, userId: data.user.id});
    chat.append(row);
    trimMessages(100);
  }

  function removeMessage(channelId, messageId) {
    const key = messageKey(channelId, messageId);
    const record = rows.get(key);
    if (record) record.element.remove();
    rows.delete(key);
  }

  function clearChat(channelId, targetUser) {
    for (const [key, record] of [...rows]) {
      if (!key.startsWith(`${channelId}:`)) continue;
      if (targetUser && record.userId !== targetUser.id) continue;
      record.element.remove();
      rows.delete(key);
    }
  }

  function eventCard(label) {
    const node = document.createElement('div');
    node.className = 'event';
    node.textContent = label;
    events.append(node);
    while (events.children.length > 50) events.firstElementChild.remove();
    const timer = setTimeout(() => { eventTimers.delete(timer); node.remove(); }, 10000);
    eventTimers.add(timer);
  }

  function trimMessages(limit) {
    while (chat.children.length > limit) {
      const oldest = chat.firstElementChild;
      for (const [key, record] of rows) if (record.element === oldest) rows.delete(key);
      oldest.remove();
    }
  }

  function reset() {
    for (const timer of eventTimers) clearTimeout(timer);
    eventTimers.clear();
    chat.replaceChildren();
    events.replaceChildren();
    rows.clear();
  }

  BokiChat.onStatus(status => { if (status === 'reset') reset(); });
  BokiChat.onEvent(event => {
    const data = event.data;
    const header = event.header;
    switch (event.type) {
    case 'ChatMessage': renderMessage(data, header); break;
    case 'MessageDeleted': removeMessage(header.channelId, data.messageId); break;
    case 'ChatCleared': clearChat(header.channelId, data.user); break;
    case 'Follow': eventCard(`${data.user.displayName} followed`); break;
    case 'Subscription': eventCard(`${data.notice.user.displayName} subscribed (${data.terms.tier})`); break;
    case 'Resubscription': eventCard(`${data.notice.user.displayName} resubscribed for ${data.cumulativeMonths} months`); break;
    case 'GiftSubscription': eventCard(`${data.gifter.user?.displayName ?? 'Anonymous'} gifted a subscription to ${data.recipient.displayName}`); break;
    case 'CommunityGiftSubscription': eventCard(`${data.gifter.user?.displayName ?? 'Anonymous'} gifted ${data.count} subscriptions`); break;
    case 'Cheer': eventCard(`${data.user?.displayName ?? 'Anonymous'} cheered ${data.bits}: ${data.text}`); break;
    case 'Raid': eventCard(`${data.from.displayName} raided with ${data.viewers} viewers`); break;
    }
  });
})();
