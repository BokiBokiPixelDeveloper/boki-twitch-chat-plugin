// Optional integration probe: actual Chromium executes the immutable imported fixture
// through the production HTTP/WebSocket runtime. Requires Node 22 and Chromium.
const {spawn} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'boki-widget-browser-'));
  const browser = spawn(process.env.WIDGET_CHROMIUM || 'chromium', ['--headless', '--no-sandbox', '--disable-gpu',
    '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'], {stdio: 'ignore'});
  const browserClosed = new Promise(resolve => browser.once('exit', resolve));
  let socket;
  const diagnostics = [];
  try {
    const portFile = path.join(profile, 'DevToolsActivePort');
    for (let i = 0; !fs.existsSync(portFile) && i < 100; ++i) await delay(100);
    const port = fs.readFileSync(portFile, 'utf8').split('\n')[0];
    const pages = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    let id = 0; const pending = new Map();
    socket.onmessage = message => {
      const frame = JSON.parse(message.data);
      if (frame.id) { const callback = pending.get(frame.id); pending.delete(frame.id); callback?.(frame); }
      else if (['Runtime.exceptionThrown', 'Log.entryAdded', 'Network.loadingFailed'].includes(frame.method)) diagnostics.push(frame);
    };
    const call = (method, params = {}) => new Promise((resolve, reject) => {
      pending.set(++id, frame => frame.error ? reject(frame.error) : resolve(frame.result));
      socket.send(JSON.stringify({id, method, params}));
    });
    await call('Page.enable'); await call('Runtime.enable'); await call('Log.enable'); await call('Network.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `window.__widgetLoads=0; window.addEventListener('onWidgetLoad',()=>window.__widgetLoads++);`});
    if (process.argv[3] === 'blocked-script') await call('Network.setBlockedURLs', {urls:['https://code.jquery.com/*']});
    await call('Page.navigate', {url: process.argv[2]});
    let result;
    for (let i = 0; i < 150; ++i) {
      await delay(100);
      result = (await call('Runtime.evaluate', {expression: `JSON.stringify({loads:window.__widgetLoads,jquery:typeof window.jQuery,api:typeof window.SE_API,rows:document.querySelectorAll('.message-row,#feed .row').length,generic:window.genericInitialized===true,state:document.readyState})`, returnByValue:true})).result.value;
      const state = JSON.parse(result);
      if (process.argv[3] === 'blocked-script' && state.state === 'complete' && diagnostics.length) break;
      if (process.argv[3] === 'generic' && state.generic && state.state === 'complete') break;
      if (state.rows > 0 && state.state === 'complete') break;
    }
    console.log(result);
    // Keep capabilities, paths and exception payloads out of test output.
    console.log(JSON.stringify(diagnostics.map(frame => ({event:frame.method, reason:frame.params.errorText || frame.params.entry?.source || 'javascript'}))));
    const state = JSON.parse(result);
    if (process.argv[3] === 'blocked-script') {
      if (state.jquery !== 'undefined' || !diagnostics.length) throw new Error('Blocked dependency was not exercised');
    } else if (diagnostics.some(frame => frame.method === 'Runtime.exceptionThrown')) {
      throw new Error('Widget execution raised a JavaScript exception');
    } else if (process.argv[3] === 'generic') {
      if (!state.generic || state.loads) throw new Error('Generic initialization failed');
    } else if (process.argv[3] === 'development') {
      if (state.rows < 1 || state.loads) throw new Error('Development widget did not render');
    } else if (state.loads !== 1 || state.rows < 1 || state.jquery !== 'function') throw new Error('Scrapbook did not initialize and render a synthetic message');
  } finally {
    if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({id:2147483647, method:'Browser.close'}));
    else browser.kill('SIGTERM');
    const stopTimeout = setTimeout(() => browser.kill('SIGKILL'), 5000);
    await browserClosed;
    clearTimeout(stopTimeout);
    socket?.close();
    // Chromium helper processes may finish their last profile write after exit.
    fs.rmSync(profile, {recursive:true, force:true, maxRetries:5, retryDelay:100});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
