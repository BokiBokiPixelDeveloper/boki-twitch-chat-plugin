const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const widgetSource = fs.readFileSync(path.join(__dirname, '..', 'resources', 'web-themes', 'development', 'widget.js'), 'utf8');

class TextNode {
  constructor(value) { this.value = String(value); this.parent = null; }
  remove() { if (this.parent) this.parent.removeChild(this); }
  get textContent() { return this.value; }
}

class Element {
  constructor(tagName, created) {
    this.tagName = tagName.toUpperCase();
    this.children = [];
    this.parent = null;
    this.style = {};
    this.created = created;
    this._text = '';
    created.push(this);
  }
  append(...nodes) { for (const node of nodes) { node.parent = this; this.children.push(node); } }
  removeChild(node) { this.children = this.children.filter(child => child !== node); node.parent = null; }
  remove() { if (this.parent) this.parent.removeChild(this); }
  replaceWith(node) {
    if (!this.parent) return;
    const index = this.parent.children.indexOf(this);
    node.parent = this.parent;
    this.parent.children[index] = node;
    this.parent = null;
  }
  replaceChildren() { for (const child of this.children) child.parent = null; this.children = []; this._text = ''; }
  addEventListener() {}
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text + this.children.map(child => child.textContent).join(''); }
  get firstElementChild() { return this.children.find(child => child instanceof Element) ?? null; }
}

function createWidget() {
  const created = [];
  const feed = new Element('main', created);
  let onEvent;
  let onStatus;
  const context = {
    document: {
      getElementById: id => id === 'feed' ? feed : null,
      createElement: tag => new Element(tag, created),
      createTextNode: value => new TextNode(value)
    },
    BokiChat: {
      onEvent(callback) { onEvent = callback; },
      onStatus(callback) { onStatus = callback; }
    },
    setTimeout() { throw new Error('The widget must not schedule expiration timers'); },
    clearTimeout() { throw new Error('The widget must not own expiration timers'); }
  };
  vm.runInNewContext(widgetSource, context, {filename: 'widget.js'});
  return {feed, created, emit: event => onEvent(event), status: value => onStatus(value)};
}

function user(id, displayName) { return {id, displayName, color: '#123456'}; }
function chatEvent(messageId, viewer, fragments, badges = [], media = []) {
  return {type: 'ChatMessage', header: {channelId: 'channel-1'}, data: {messageId, user: viewer, fragments, badges, media}};
}
function notice(viewer, messageText) {
  return {user: viewer, text: messageText, fragments: [{type: 'text', text: messageText}], badges: [], media: []};
}
function follow(viewer) {
  return {type: 'Follow', header: {channelId: 'channel-1'}, data: {user: viewer}};
}

test('hostile semantic text creates only text nodes', () => {
  const widget = createWidget();
  const values = ['<script>alert(1)</script>', '<img src=x onerror=alert(1)>', '<svg onload=alert(1)>'];
  widget.emit(chatEvent('message-1', user('user-1', '<b>Viewer</b>'), values.map(value => ({type: 'text', text: value, imageUrl: null}))));
  const hostileNotice = notice(user('user-1', 'Viewer'), '<img src=x onerror=alert(2)>');
  widget.emit({type: 'Subscription', header: {channelId: 'channel-1'}, data: {notice: hostileNotice, terms: {tier: 'tier1'}}});
  for (const value of [...values, hostileNotice.text]) assert.match(widget.feed.textContent, new RegExp(value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
  assert.equal(widget.created.some(node => ['SCRIPT', 'SVG'].includes(node.tagName)), false);
  assert.equal(widget.created.some(node => node.tagName === 'IMG'), false);
});

test('structured emotes, resolved badges, and media create controlled images', () => {
  const widget = createWidget();
  widget.emit(chatEvent('message-1', user('user-1', 'Viewer'), [
    {type: 'text', text: 'Hello ', imageUrl: null},
    {type: 'emote', text: 'Kappa', imageUrl: 'https://static-cdn.jtvnw.net/emote.png'}
  ], [{type: 'moderator', imageUrl: 'https://static-cdn.jtvnw.net/badge.png'}],
  [{imageUrl: 'https://cdn.betterttv.net/media.gif'}]));
  assert.deepEqual(widget.created.filter(node => node.tagName === 'IMG').map(node => node.src), [
    'https://static-cdn.jtvnw.net/badge.png',
    'https://static-cdn.jtvnw.net/emote.png',
    'https://cdn.betterttv.net/media.gif'
  ]);
});

test('unresolved badges retain readable text fallback', () => {
  const widget = createWidget();
  widget.emit(chatEvent('message-1', user('user-1', 'Viewer'), [{type: 'text', text: 'Hello'}],
    [{type: 'broadcaster', imageUrl: null}, {type: 'subscriber', imageUrl: null}]));
  assert.match(widget.feed.textContent, /broadcaster/);
  assert.match(widget.feed.textContent, /subscriber/);
  assert.equal(widget.created.some(node => node.tagName === 'IMG'), false);
});

test('chat and channel events retain one chronological order without expiration', () => {
  const widget = createWidget();
  const actor = user('user-1', 'Viewer');
  widget.emit(chatEvent('a', actor, [{type: 'text', text: 'Chat A'}]));
  widget.emit({type: 'Subscription', header: {channelId: 'channel-1'}, data: {
    notice: notice(actor, 'Subscription message'), terms: {tier: 'tier1'}}});
  widget.emit(chatEvent('b', actor, [{type: 'text', text: 'Chat B'}]));
  widget.emit({type: 'Cheer', header: {channelId: 'channel-1'}, data: {user: actor, bits: 100, text: 'Cheer text'}});
  assert.deepEqual(widget.feed.children.map(row => row.textContent), [
    'ViewerChat A', 'Viewer subscribed (tier1) — Subscription message', 'ViewerChat B', 'Viewer cheered 100: Cheer text'
  ]);
  assert.equal(widget.feed.children.length, 4);
});

test('subscription and resubscription render accompanying semantic text', () => {
  const widget = createWidget();
  const actor = user('user-1', 'Viewer');
  widget.emit({type: 'Subscription', header: {channelId: 'channel-1'}, data: {
    notice: notice(actor, 'First subscription message'), terms: {tier: 'tier1'}}});
  widget.emit({type: 'Resubscription', header: {channelId: 'channel-1'}, data: {
    notice: notice(actor, 'Glad to be back!'), cumulativeMonths: 3}});
  assert.match(widget.feed.children[0].textContent, /First subscription message/);
  assert.match(widget.feed.children[1].textContent, /Glad to be back!/);
});

test('delete and clear affect chat rows without removing adjacent events', () => {
  const widget = createWidget();
  widget.emit(chatEvent('one', user('user-1', 'Same name'), [{type: 'text', text: 'one'}]));
  widget.emit(follow(user('event-user', 'Follower')));
  widget.emit(chatEvent('two', user('user-2', 'Same name'), [{type: 'text', text: 'two'}]));
  widget.emit({type: 'MessageDeleted', header: {channelId: 'channel-1'}, data: {messageId: 'one'}});
  assert.deepEqual(widget.feed.children.map(row => row.textContent), ['Follower followed', 'Same nametwo']);
  widget.emit({type: 'ChatCleared', header: {channelId: 'channel-1'}, data: {user: user('user-1', 'Same name')}});
  assert.equal(widget.feed.children.length, 2);
  widget.emit({type: 'ChatCleared', header: {channelId: 'channel-1'}, data: {user: null}});
  assert.deepEqual(widget.feed.children.map(row => row.textContent), ['Follower followed']);
});

test('all channel event types create entries in the feed', () => {
  const widget = createWidget();
  const actor = user('user-1', 'Viewer');
  const message = notice(actor, 'Notice text');
  const header = {channelId: 'channel-1'};
  const events = [
    {type: 'Follow', data: {user: actor}},
    {type: 'Subscription', data: {notice: message, terms: {tier: 'tier1'}}},
    {type: 'Resubscription', data: {notice: message, cumulativeMonths: 3}},
    {type: 'GiftSubscription', data: {notice: message, gifter: {user: actor}, recipient: user('user-2', 'Recipient')}},
    {type: 'CommunityGiftSubscription', data: {notice: message, gifter: {user: actor}, count: 5}},
    {type: 'Cheer', data: {user: actor, bits: 100, text: 'Great stream'}},
    {type: 'Raid', data: {from: actor, viewers: 42}}
  ];
  for (const event of events) widget.emit({...event, header});
  assert.equal(widget.feed.children.length, 7);
});

test('two widget contexts retain independent DOM state', () => {
  const first = createWidget();
  const second = createWidget();
  const event = chatEvent('message-1', user('user-1', 'Viewer'), [{type: 'text', text: 'Hello'}]);
  first.emit(event);
  second.emit(event);
  first.emit(follow(user('user-2', 'Follower')));
  assert.equal(first.feed.children.length, 2);
  assert.equal(second.feed.children.length, 1);
  first.status('reset');
  assert.equal(first.feed.children.length, 0);
  assert.equal(second.feed.children.length, 1);
});

test('widget source contains no executable HTML sinks or string timers', () => {
  for (const sink of ['innerHTML', 'outerHTML', 'insertAdjacentHTML', 'document.write', 'eval(', 'new Function', 'setTimeout(', 'setInterval('])
    assert.equal(widgetSource.includes(sink), false, sink);
});
