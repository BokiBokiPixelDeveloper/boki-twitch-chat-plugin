(() => {
  'use strict';
  const feed = document.getElementById('feed');
  const rows = new Map();
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

  function appendBadges(row, badges = []) {
    for (const badge of badges) {
      if (badge.imageUrl) row.append(image(badge.imageUrl, badge.type, 'badge-image'));
      else {
        const label = document.createElement('span');
        label.className = 'badge';
        label.textContent = badge.type;
        row.append(label);
      }
    }
  }

  function appendFragments(row, fragments = []) {
    for (const fragment of fragments) {
      row.append(fragment.type === 'emote' && fragment.imageUrl
        ? image(fragment.imageUrl, fragment.text, 'emote') : text(fragment.text));
    }
  }

  function appendMedia(row, media = []) {
    for (const asset of media) {
      if (asset.imageUrl) row.append(image(asset.imageUrl, 'Media', 'media'));
    }
  }

  function appendNotice(row, notice) {
    if (!notice) return;
    appendBadges(row, notice.badges);
    if (notice.fragments?.length) {
      row.append(text(' — '));
      appendFragments(row, notice.fragments);
    } else if (notice.text) {
      row.append(text(` — ${notice.text}`));
    }
    appendMedia(row, notice.media);
  }

  function appendRow(row) {
    feed.append(row);
    trimFeed(100);
  }

  function renderMessage(data, header) {
    const row = document.createElement('div');
    row.className = 'row';
    appendBadges(row, data.badges);
    const name = document.createElement('span');
    name.className = 'name';
    name.textContent = data.user.displayName;
    // Map validated Twitch colors to external CSS classes under style-src 'self'.
    if (/^#[0-9a-f]{6}$/i.test(data.user.color || '')) {
      const value = parseInt(data.user.color.slice(1), 16);
      const rgb = [value >> 16, (value >> 8) & 255, value & 255];
      const palette = [[255,128,128], [255,192,128], [255,255,128], [128,255,128],
                       [128,255,255], [128,160,255], [192,128,255], [255,128,192]];
      const distance = color => color.reduce((sum, channel, i) => sum + (channel - rgb[i]) ** 2, 0);
      const closest = palette.reduce((best, color, i) => distance(color) < distance(palette[best]) ? i : best, 0);
      name.className += ` name-color-${closest}`;
    }
    row.append(name);
    appendFragments(row, data.fragments);
    appendMedia(row, data.media);
    const key = messageKey(header.channelId, data.messageId);
    const previous = rows.get(key);
    if (previous) previous.element.remove();
    rows.set(key, {element: row, userId: data.user.id});
    appendRow(row);
  }

  function renderEvent(label, notice) {
    const row = document.createElement('div');
    row.className = 'event';
    row.append(text(label));
    appendNotice(row, notice);
    appendRow(row);
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

  function trimFeed(limit) {
    while (feed.children.length > limit) {
      const oldest = feed.firstElementChild;
      for (const [key, record] of rows) if (record.element === oldest) rows.delete(key);
      oldest.remove();
    }
  }

  function reset() {
    feed.replaceChildren();
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
    case 'Follow': renderEvent(`${data.user.displayName} followed`); break;
    case 'Subscription': renderEvent(`${data.notice.user.displayName} subscribed (${data.terms.tier})`, data.notice); break;
    case 'Resubscription': renderEvent(`${data.notice.user.displayName} resubscribed for ${data.cumulativeMonths} months`, data.notice); break;
    case 'GiftSubscription': renderEvent(`${data.gifter.user?.displayName ?? 'Anonymous'} gifted a subscription to ${data.recipient.displayName}`, data.notice); break;
    case 'CommunityGiftSubscription': renderEvent(`${data.gifter.user?.displayName ?? 'Anonymous'} gifted ${data.count} subscriptions`, data.notice); break;
    case 'Cheer': renderEvent(`${data.user?.displayName ?? 'Anonymous'} cheered ${data.bits}: ${data.text}`); break;
    case 'Raid': renderEvent(`${data.from.displayName} raided with ${data.viewers} viewers`); break;
    }
  });
})();
