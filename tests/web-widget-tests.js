const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const widgetSource = fs.readFileSync(path.join(__dirname, '..', 'resources', 'web-themes', 'development', 'widget.js'), 'utf8');

class TextNode {
  constructor(value) { this.value = String(value); this.parent = null; }
  remove() { if (this.parent) this.parent.removeChild(this); }
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
  get textContent() { return this._text + this.children.map(child => child.textContent ?? child.value).join(''); }
  get firstElementChild() { return this.children.find(child => child instanceof Element) ?? null; }
}

function createWidget() {
  const created = [];
  const chat = new Element('section', created);
  const events = new Element('main', created);
  let onEvent;
  let onStatus;
  let timerId = 0;
  const context = {
    document: {
      getElementById: id => id === 'chat' ? chat : events,
      createElement: tag => new Element(tag, created),
      createTextNode: value => new TextNode(value)
    },
    BokiChat: {
      onEvent(callback) { onEvent = callback; },
      onStatus(callback) { onStatus = callback; }
    },
    setTimeout() { return ++timerId; },
    clearTimeout() {}
  };
  vm.runInNewContext(widgetSource, context, {filename: 'widget.js'});
  return {chat, events, created, emit: event => onEvent(event), status: value => onStatus(value)};
}

function user(id, displayName) { return {id, displayName, color: '#123456'}; }
function chatEvent(messageId, viewer, fragments, badges = []) {
  return {type: 'ChatMessage', header: {channelId: 'channel-1'}, data: {messageId, user: viewer, fragments, badges}};
}

test('hostile semantic text creates only text nodes', () => {
  const widget = createWidget();
  const values = ['<script>alert(1)</script>', '<img src=x onerror=alert(1)>', '<svg onload=alert(1)>'];
  widget.emit(chatEvent('message-1', user('user-1', '<b>Viewer</b>'), values.map(value => ({type: 'text', text: value, imageUrl: null}))));
  assert.equal(widget.chat.children.length, 1);
  for (const value of values) assert.match(widget.chat.children[0].textContent, new RegExp(value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
  assert.equal(widget.created.some(node => ['SCRIPT', 'SVG'].includes(node.tagName)), false);
  assert.equal(widget.created.some(node => node.tagName === 'IMG'), false);
});

test('structured emotes and badges create controlled images', () => {
  const widget = createWidget();
  widget.emit(chatEvent('message-1', user('user-1', 'Viewer'), [
    {type: 'text', text: 'Hello ', imageUrl: null},
    {type: 'emote', text: 'Kappa', imageUrl: 'https://static-cdn.jtvnw.net/emote.png'}
  ], [{type: 'moderator', imageUrl: 'https://static-cdn.jtvnw.net/badge.png'}]));
  const images = widget.created.filter(node => node.tagName === 'IMG');
  assert.equal(images.length, 2);
  assert.deepEqual(images.map(image => image.src), [
    'https://static-cdn.jtvnw.net/badge.png',
    'https://static-cdn.jtvnw.net/emote.png'
  ]);
  assert.match(widget.chat.children[0].textContent, /Hello /);
});

test('delete and clear use message, channel, and user IDs', () => {
  const widget = createWidget();
  widget.emit(chatEvent('one', user('user-1', 'Same name'), [{type: 'text', text: 'one'}]));
  widget.emit(chatEvent('two', user('user-2', 'Same name'), [{type: 'text', text: 'two'}]));
  widget.emit({type: 'MessageDeleted', header: {channelId: 'channel-1'}, data: {messageId: 'one'}});
  assert.equal(widget.chat.children.length, 1);
  widget.emit({type: 'ChatCleared', header: {channelId: 'channel-1'}, data: {user: user('user-1', 'Same name')}});
  assert.equal(widget.chat.children.length, 1);
  widget.emit({type: 'ChatCleared', header: {channelId: 'channel-1'}, data: {user: null}});
  assert.equal(widget.chat.children.length, 0);
});

test('all channel event types create visible cards', () => {
  const widget = createWidget();
  const actor = user('user-1', 'Viewer');
  const notice = {user: actor};
  const header = {channelId: 'channel-1'};
  const events = [
    {type: 'Follow', data: {user: actor}},
    {type: 'Subscription', data: {notice, terms: {tier: 'tier1'}}},
    {type: 'Resubscription', data: {notice, cumulativeMonths: 3}},
    {type: 'GiftSubscription', data: {gifter: {user: actor}, recipient: user('user-2', 'Recipient')}},
    {type: 'CommunityGiftSubscription', data: {gifter: {user: actor}, count: 5}},
    {type: 'Cheer', data: {user: actor, bits: 100, text: 'Great stream'}},
    {type: 'Raid', data: {from: actor, viewers: 42}}
  ];
  for (const event of events) widget.emit({...event, header});
  assert.equal(widget.events.children.length, 7);
});

test('two widget contexts retain independent DOM state', () => {
  const first = createWidget();
  const second = createWidget();
  const event = chatEvent('message-1', user('user-1', 'Viewer'), [{type: 'text', text: 'Hello'}]);
  first.emit(event);
  second.emit(event);
  assert.equal(first.chat.children.length, 1);
  assert.equal(second.chat.children.length, 1);
  first.status('reset');
  assert.equal(first.chat.children.length, 0);
  assert.equal(second.chat.children.length, 1);
});

test('widget source contains no executable HTML sinks', () => {
  for (const sink of ['innerHTML', 'outerHTML', 'insertAdjacentHTML', 'document.write', 'eval(', 'new Function'])
    assert.equal(widgetSource.includes(sink), false, sink);
});
