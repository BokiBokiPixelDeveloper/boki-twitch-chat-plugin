const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'resources', 'web-runtime', 'streamelements-adapter.js'), 'utf8');
function adapter() {
  const listeners = new Map(); const dispatched = []; const warnings = []; let eventListener;
  class CustomEvent { constructor(type, init) { this.type = type; this.detail = init.detail; } }
  const context = {window: {BokiStreamElementsConfig: {packageId: 'package', instanceId: 'instance', fieldData: {badgesDisplay: true}, channel: {username: 'channel'}, currency: {symbol: ''}},
    addEventListener(type, callback) { listeners.set(type, callback); }, dispatchEvent(value) { dispatched.push(value); return true; }},
    CustomEvent, BokiChat: {ready: Promise.resolve(), onEvent(callback) { eventListener = callback; }}, console: {warn(value) { warnings.push(value); }}, Promise, Object, String, Number, Boolean, Date, Array, Proxy};
  vm.runInNewContext(source, context, {filename: 'streamelements-adapter.js'});
  return Promise.resolve().then(() => ({context, dispatched, warnings, emit: value => eventListener(value)}));
}
const header = {timestamp: '2026-01-01T00:00:00.000Z'};
const user = {id: 'user/1', login: 'viewer', displayName: 'Viewer', color: '#123456'};
const decodeHtmlText = value => value.replace(/&#(38|60|62|34|39);/g, (_, code) => String.fromCodePoint(Number(code)));
test('loads Scrapbook fields and maps chat, emotes, and resolved badges', async () => {
  const widget = await adapter();
  assert.equal(widget.dispatched[0].type, 'onWidgetLoad'); assert.equal(widget.dispatched[0].detail.fieldData.badgesDisplay, true);
  widget.emit({type: 'ChatMessage', header, data: {messageId: 'message/1', user, text: 'Kappa', metadata: {}, badges: [{type: 'broadcaster', version: '1', imageUrl: 'https://static-cdn.jtvnw.net/badge.png'}],
    fragments: [{type: 'emote', text: 'Kappa', emoteId: '25', provider: 'twitch', imageUrl: 'https://static-cdn.jtvnw.net/emote.gif', sourceRange: {offset: 0, length: 5}, twitch: {supportsAnimated: true}}]}});
  const event = widget.dispatched.at(-1); assert.equal(event.detail.listener, 'message');
  assert.equal(event.detail.event.data.msgId, 'message-message_2f_1'); assert.equal(event.detail.event.data.userId, 'user-user_2f_1');
  assert.equal(event.detail.event.data.badges[0].url, 'https://static-cdn.jtvnw.net/badge.png');
  assert.equal(event.detail.event.data.emotes[0].urls[4], 'https://static-cdn.jtvnw.net/emote.gif'); assert.equal(event.detail.event.data.emotes[0].gif, true);
});
test('maps every required channel and deletion listener without constructing code', async () => {
  const widget = await adapter();
  const notice = {user, text: '<b>viewer text</b>'};
  const events = [
    {type: 'MessageDeleted', data: {messageId: 'm'}}, {type: 'ChatCleared', data: {user}}, {type: 'ChatCleared', data: {user: null}},
    {type: 'Follow', data: {user}}, {type: 'Subscription', data: {notice, terms: {tier: 'tier1'}}}, {type: 'Resubscription', data: {notice, cumulativeMonths: 3, streakMonths: 2, terms: {tier: 'tier1'}}},
    {type: 'GiftSubscription', data: {notice, recipient: user, gifter: {user}, communityGiftId: 'group', terms: {tier: 'tier1'}}},
    {type: 'CommunityGiftSubscription', data: {notice, count: 5, gifter: {user}, tier: 'tier1'}}, {type: 'Cheer', data: {user, bits: 10, text: 'Cheer10'}}, {type: 'Raid', data: {from: user, viewers: 12}}
  ];
  for (const value of events) widget.emit({...value, header});
  assert.deepEqual(widget.dispatched.slice(1).map(value => value.detail.listener), ['delete-message', 'delete-messages', 'follower-latest', 'subscriber-latest', 'subscriber-latest', 'subscriber-latest', 'subscriber-latest', 'cheer-latest', 'raid-latest']);
  assert.equal(widget.dispatched[4].detail.event.message, '&#60;b&#62;viewer text&#60;/b&#62;');
  assert.equal(widget.dispatched[8].detail.event.amount, 10); assert.equal(widget.dispatched[9].detail.event.amount, 12);
});
test('protects StreamElements HTML text sinks without double encoding chat text', async () => {
  const hostileNames = [
    '<script>alert(1)</script>', '<img src=x onerror=alert(1)>', '<svg onload=alert(1)>',
    '"><img src=x onerror=alert(1)>', "'", '"', '&', '<', '>', 'Boki', 'Test_User', 'Änne', '猫',
    '👩🏽‍💻', 'A\u0308nne'
  ];
  for (const displayName of hostileNames) {
    const widget = await adapter();
    const hostileUser = {...user, login: displayName, displayName};
    widget.emit({type: 'ChatMessage', header, data: {messageId: 'message', user: hostileUser,
      text: '<b>chat & text</b>', metadata: {}, badges: [{type: 'subscriber', version: '1', info: displayName}], fragments: []}});
    const data = widget.dispatched.at(-1).detail.event.data;
    assert.equal(decodeHtmlText(data.displayName), displayName);
    assert.equal(decodeHtmlText(data.nick), displayName);
    assert.equal(decodeHtmlText(data.badges[0].description), displayName);
    assert.equal(data.text, '<b>chat & text</b>');
    assert.doesNotMatch(data.displayName, /[<>]/);
    assert.doesNotMatch(`<span class="name">${data.displayName}</span>`, /<(?:script|img|svg)\b/i);
  }

  const widget = await adapter();
  const hostile = '<img src=x onerror=alert(1)> & "quoted"';
  const hostileUser = {...user, displayName: hostile};
  const notice = {user: hostileUser, text: hostile};
  widget.emit({type: 'Subscription', header, data: {notice, terms: {tier: 'tier1'}}});
  const subscription = widget.dispatched.at(-1).detail.event;
  assert.equal(decodeHtmlText(subscription.name), hostile);
  assert.equal(decodeHtmlText(subscription.message), hostile);
  assert.doesNotMatch(subscription.name + subscription.message, /[<>]/);
  widget.emit({type: 'Cheer', header, data: {user: hostileUser, bits: 10, text: hostile}});
  const cheer = widget.dispatched.at(-1).detail.event;
  assert.equal(decodeHtmlText(cheer.name), hostile);
  assert.equal(decodeHtmlText(cheer.message), hostile);
  assert.doesNotMatch(cheer.name + cheer.message, /[<>]/);
});
test('unsupported SE API access logs and rejects without fabricated state', async () => {
  const widget = await adapter(); const promise = widget.context.window.SE_API.store.get('key');
  await assert.rejects(promise); assert.match(widget.warnings.at(-1), /Unsupported API call: SE_API.store.get/);
  assert.equal(widget.context.window.SE_API.cheerFilter('Cheer100'), 'Cheer100');
  assert.equal(widget.context.window.SE_API.sanitize({message: '<img onerror=x>'}), '&#60;img onerror=x&#62;');
});
