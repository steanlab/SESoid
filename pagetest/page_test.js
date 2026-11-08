// Page test: headless Chromium with a mock Web Bluetooth tag (simulates the V300 protocol)
const { chromium } = require('playwright');
const path = require('path');
const PAGE = 'file://' + process.argv[2];
const OUT = process.argv[3];

const MOCK = `
(() => {
  const crcT = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
  const crc = b => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  const le32 = v => [v & 255, v >>> 8 & 255, v >>> 16 & 255, v >>> 24 & 255];
  const tag = { flash: new Uint8Array(8 * 65536).fill(255), cfg: { en: 1, mode: 0, iv: 60, mask: 0xFE, temp: 20, tx: 6 }, cur: 0, src: 1, shown: 0,
    up: null, log: [], dropPage: 3, dropped: false, rebooted: 0, fills: [] };
  window.__tag = tag;
  let notify = null;
  const N = a => setTimeout(() => notify && notify({ target: { value: new DataView(new Uint8Array(a).buffer) } }), 2);
  function slotInfo(s) { const o = s * 65536 + 0xF000; const r = tag.flash.subarray(o, o + 256);
    if (r[0] !== 0x53 || r[1] !== 0x4C || r[2] !== 0x44 || r[3] !== 0x31) return { valid: 0, total: 0, crc: 0 };
    const dv = new DataView(r.buffer, r.byteOffset); return { valid: 1, total: dv.getUint32(4, true), crc: dv.getUint32(8, true) }; }
  function handle(v) {
    const op = v[0];
    if (op === 1) N([0x81, ...v.slice(1)]);
    else if (op === 0x10) {
      const mode = v[1], total = v[2] | v[3] << 8 | v[4] << 16 | v[5] << 24 >>> 0, slot = mode ? v[6] : 0;
      tag.up = { mode, total, slot, buf: new Uint8Array(total), next: 0, mask: {}, got: 0 };
      if (mode) { tag.flash.fill(255, slot * 65536, slot * 65536 + 65536); setTimeout(() => N([0x90, 0, 1, slot]), 300); } else N([0x90, 0, 0]);
    } else if (op === 0x11) {
      const idx = v[1] | v[2] << 8, u = tag.up; if (!u) return; const page = idx >> 4;
      if (page < u.next) return; u.buf.set(v.slice(3, 19).subarray(0, Math.max(0, Math.min(16, u.total - idx * 16))), idx * 16);
      (u.mask[page] = u.mask[page] || new Set()).add(idx & 15);
    } else if (op === 0x12) {
      const u = tag.up, page = v[1] | v[2] << 8; if (!u) return;
      if (page < u.next) { N([0x92, page & 255, page >> 8, 0]); return; }
      if (page >= u.next + 4) { N([0x92, page & 255, page >> 8, 1, u.next & 255, u.next >> 8]); return; }
      const n = Math.min(256, u.total - page * 256), need = Math.ceil(n / 16), have = u.mask[page] || new Set();
      if (page === tag.dropPage && !tag.dropped) { tag.dropped = true; have.delete(5); }
      if (have.size < need) { let m = 0; for (const k of have) m |= 1 << k; N([0x93, page & 255, page >> 8, m & 255, m >> 8]); return; }
      if (page !== u.next) { N([0x92, page & 255, page >> 8, 1, u.next & 255, u.next >> 8]); return; }
      u.next++; u.got += n; N([0x92, page & 255, page >> 8, 0]);
    } else if (op === 0x13) {
      const u = tag.up; if (!u) return;
      const ok = u.next * 256 >= u.total, c = crc(u.buf);
      if (ok && u.mode) { const o = u.slot * 65536; tag.flash.set(u.buf, o); const rec = new Uint8Array(256).fill(255); rec.set([0x53, 0x4C, 0x44, 0x31, ...le32(u.total), ...le32(c), 1, 0, 0, 0], 0); tag.flash.set(rec, o + 0xF000); }
      N([0x94, ok ? 0 : 1, ...le32(c), ...le32(u.total)]);
    } else if (op === 0x24) {
      const s = v[1], i = slotInfo(s); if (!i.valid) { N([0xA4, s, 1, 0, 0, 0, 0, 0, 0, 0, 0]); return; }
      const c = crc(tag.flash.subarray(s * 65536, s * 65536 + i.total)); N([0xA4, s, c === i.crc ? 0 : 2, ...le32(i.total), ...le32(c)]);
    } else if (op === 0x30) {
      N([0xB0, 2, 0, tag.src, tag.shown, 27, ...le32(1800), ...le32(7200), 5, 0, 0, 3 | 0]);
    } else if (op === 0x31) {
      for (let s = 1; s <= 7; s++) { const i = slotInfo(s); N([0xB1, s, (i.valid ? 1 : 0) | (i.valid && i.total === 46080 ? 2 : 0) | (tag.cfg.mask >> s & 1 ? 4 : 0) | (tag.cur === s ? 8 : 0), ...le32(i.total), ...le32(i.crc)]); }
    } else if (op === 0x32) { tag.cur = v[1]; tag.src = 2; tag.shown = v[1]; N([0xB3, 0x32, 0]); }
    else if (op === 0x33) N([0xB3, 0x33, 0]);
    else if (op === 0x34) { tag.cfg = { en: v[1], mode: v[2], iv: v[3] | v[4] << 8, mask: v[5], temp: v[6] << 24 >> 24, tx: v[7] << 24 >> 24 }; sendCfg(); }
    else if (op === 0x35) sendCfg();
    else if (op === 0x36) { tag.rebooted++; N([0xB5]); }
    else if (op === 0x37) {
      N([0xB6, 0, 1, 0, 0x08, 0, 0, 0, 0x20, 0x1C, 0, 0, 0x30, 0x0F, 0x20, 0x00]);
      N([0xB6, 1, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0, 1, 32, 0]);
      N([0xB6, 2, 0xEF, 0x40, 0x13, 0x04, 0x0D, 0, 0xC8, 0x4B, 0, 0, 7, 0, 0]);
      N([0xB6, 3, 0x40, 0x0D, 0x03, 0, 3, 0, 0xC4, 60, 0, 1, 2]);
    }
    else if (op === 0x38) { tag.fills.push(v[1]); N([0xB3, 0x38, 0]); }
    else if (op === 0x39) N([0xB3, 0x39, 0]);
    else if (op === 0x20) N([0xA0, 0xEF, 0x40, 0x13, 0, 0, 0]);
    else if (op === 0x3A) { N([0xB3, 0x3A, 0]); setTimeout(() => N(tag.dead ? [0xB8, 2, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 10, 0, 0, 0, 0, 0, 0, 0] : [0xB8, 2, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 10, 0, 0x04, 0x0D, 0x04, 0x0D, 0x04, 0x0D]), 100); }
    else if (op === 0x3C) { N([0xB3, 0x3C, 0]); setTimeout(() => N(clkPkt()), 100); }
    else if (op === 0x3D) N(clkPkt(true));
    else if (op === 0x3E && tag.old11 && v.length !== 2) N([0xB3, 0x3E, 2]);
    else if (op === 0x3E) { tag.pmMask = v.length >= 6 ? (v[2] | v[3] << 8 | v[4] << 16 | v[5] << 24) >>> 0 : 0xFFFFFFFF; tag.pmLen = v.length; tag.pm = { mode: v[1] & 3, swd: v[1] & 0x80, level: 0, hi: 0, lo: 0, edges: 0, last: 255, el: 0 }; tag.pmBase(); N(pmPkt()); }
    else if (op === 0x3F && v[1] === 2) { tag.scanArgs = Array.from(v); N([0xB3, 0x3F, tag.scanOk === false ? 2 : 0]); if (tag.scanOk !== false) setTimeout(() => N(scanPkt(2)), 100); }
    else if (op === 0x3F && v[1] === 3) N(scanPkt(2));
    else if (op === 0x3F && v[1] === 4) N(scanPkt(3));
    else if (op === 0x3F) { if (v[1] === 1) { tag.pm.hi = tag.pm.lo = 0; tag.pm.edges = 0; tag.pm.last = 255; tag.pmBase(); } N(pmPkt()); }
    else if (op === 0x3B) N([0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  }
  const PMASK = (1 << 6) | (1 << 8) | (0xF << 10) | (1 << 19) | (1 << 21) | (0xF << 22);
  tag.pm = { mode: 0, swd: 0, level: 0, hi: 0, lo: 0, edges: 0, last: 255, el: 0 };
  tag.xtal = true;
  tag.pmMask = 0xFFFFFFFF;
  tag.pmBase = () => { const m = tag.pm, W = (PMASK & tag.pmMask) >>> 0; m.level = m.mode === 2 ? W : 0; m.hi = m.level; m.lo = W & ~m.level; };
  function pmPkt() { const m = tag.pm; return [0xBA, m.mode | m.swd, m.el & 255, m.el >> 8, ...le32(m.level >>> 0), ...le32(m.hi >>> 0), ...le32(m.lo >>> 0), m.edges & 255, m.edges >> 8, m.last, 0]; }
  tag.pmPush = () => N(pmPkt());
  function scanPkt(st) {
    const hit = tag.scanHit !== false;
    return [0xBB, st, tag.scanArgs ? tag.scanArgs[2] : 0, 6, 6, 4, hit ? 1 : 0, hit ? 24 : 0, hit ? 25 : 0, hit ? 0x55 : 0, 0, 0, 0, 0, 0, 0, ...le32(((1 << 24) | (1 << 25)) >>> 0)];
  }
  function clkPkt(never) {
    if (never) return [0xB9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    const c = tag.xtal ? 299994 : 0, r = 303030, u = 279272;
    const fl = tag.xtal ? 0x01 | 0x04 | 0x10 | 0x20 | 0x40 : 0x01 | 0x80 | 0x20 | 0x40;
    return [0xB9, 2, fl, 3 | (1 << 3) | (1 << 5), 0x8C, 0x7A, 0x8C, 0x7A, tag.xtal ? 0x93 : 0xFF, tag.xtal ? 0x01 : 0xFF, c & 255, c >> 8 & 255, c >> 16, r & 255, r >> 8 & 255, r >> 16, u & 255, u >> 8 & 255, u >> 16, 0x30];
  }
  function sendCfg() { const c = tag.cfg; N([0xB4, 1, c.en, c.mode, c.iv & 255, c.iv >> 8, c.mask, c.temp & 255, c.tx & 255, 60, 0]); }
  const chr = { addEventListener: (n, f) => { notify = f; }, startNotifications: async () => {}, writeValueWithResponse: async v => handle(new Uint8Array(v.buffer ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : v)), writeValueWithoutResponse: async v => handle(new Uint8Array(v.buffer ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : v)) };
  const svc = { getCharacteristic: async () => chr };
  const device = { name: 'V300-TEST', gatt: { connected: false, connect: async function () { this.connected = true; return { getPrimaryService: async () => svc }; }, disconnect: function () { this.connected = false; device.__d && device.__d(); } }, addEventListener: (n, f) => { device.__d = f; } };
  Object.defineProperty(navigator, 'bluetooth', { value: { requestDevice: async () => device }, configurable: true });
})();
`;

(async () => {
  const br = await chromium.launch();
  const ctx = await br.newContext({ viewport: { width: 1200, height: 900 }, deviceScaleFactor: 1 });
  const pg = await ctx.newPage();
  const errs = [];
  pg.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
  pg.on('pageerror', e => errs.push('pageerror: ' + e.message));
  pg.on('dialog', d => d.accept());
  await pg.addInitScript(MOCK);
  await pg.goto(PAGE);
  const fails = [];
  const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fails.push(m); };

  // --- converter
  await pg.click('#bDemo');
  await pg.waitForFunction(() => window.__gs.lastFrame, null, { timeout: 20000 });
  await pg.waitForTimeout(500);
  const r1 = await pg.evaluate(() => {
    const g = window.__gs, f = g.lastFrame, idx = g.lastIdx;
    // roundtrip check frame<->idx for both orientations, counts
    const back = g.frameToIdx(f, g.P.orient); let same = true; for (let i = 0; i < idx.length; i++) if (back[i] !== idx[i]) { same = false; break; }
    const cnt = [0, 0, 0, 0]; for (const v of idx) cnt[v]++;
    // spot check of mapping, FPC-left: line=u, pixel=255-v
    const u = 100, v = 37, p = 255 - v, code = (f[u * 64 + (p >> 2)] >> (6 - 2 * (p & 3))) & 3;
    const g2 = g.buildFrame(idx, 'right'), u2 = 100, v2 = 37, line = 719 - u2, code2 = (g2[line * 64 + (v2 >> 2)] >> (6 - 2 * (v2 & 3))) & 3;
    return { len: f.length, same, cnt, mapOk: code === idx[v * 720 + u], mapOk2: code2 === idx[v2 * 720 + u2], crc: g.crc32(f) };
  });
  ok(r1.len === 46080, 'frame 46080 bytes');
  ok(r1.same, 'frame<->index roundtrip');
  ok(r1.mapOk && r1.mapOk2, 'FPC left/right mapping spot check');
  ok(r1.cnt.every(c => c > 1000), 'all four colours used in demo: ' + r1.cnt.join(','));
  await pg.screenshot({ path: path.join(OUT, 'shot_edit_vivid.png') });

  // presets & algorithms run without error and keep frame size
  for (const k of ['photo', 'graphic', 'mono', 'retro', 'vivid']) {
    await pg.click('#presets button[data-k=' + k + ']'); await pg.waitForTimeout(300);
    await pg.waitForFunction(() => !document.querySelector('#stat').textContent.includes('NaN'));
  }
  for (const a of ['none', 'fs', 'jjn', 'stucki', 'burkes', 'atkinson', 'sierralite', 'bayer4', 'bayer8', 'noise']) {
    await pg.evaluate(a => { window.__gs.P.algo = a; document.querySelector('[id=t-edit]'); }, a);
    await pg.evaluate(() => window.__gs.convert()); await pg.waitForTimeout(150);
    const n = await pg.evaluate(() => window.__gs.lastFrame.length); if (n !== 46080) ok(false, 'algo size ' + a);
  }
  ok(true, 'all dithering algorithms ran');
  // mono preset leaves only K/W
  await pg.click('#presets button[data-k=mono]'); await pg.waitForTimeout(600);
  const mono = await pg.evaluate(() => { const c = [0, 0, 0, 0]; for (const v of window.__gs.lastIdx) c[v]++; return c; });
  ok(mono[2] === 0 && mono[3] === 0 && mono[0] > 1000 && mono[1] > 1000, 'mono preset: only black and white ' + mono.join(','));
  await pg.click('#presets button[data-k=vivid]'); await pg.waitForTimeout(600);

  // --- connect
  await pg.click('#bConn'); await pg.waitForTimeout(1200);
  ok((await pg.textContent('#connTxt')).includes('V300-TEST'), 'connected');
  ok(await pg.evaluate(() => !!window.__gs.tag.cfg), 'cfg received (B4)');
  ok(await pg.evaluate(() => !!window.__gs.tag.st), 'status received (B0)');

  // --- upload to slot 3
  await pg.selectOption('#upSlot', '3');
  await pg.evaluate(() => window.__gs.convert()); await pg.waitForTimeout(300);
  const want = await pg.evaluate(() => window.__gs.crc32(window.__gs.lastFrame));
  const t0 = Date.now();
  await pg.click('#bUpload');
  await pg.waitForFunction(() => /ГОТОВО|ОШИБКА|Прервано/.test(document.querySelector('#upRes').textContent), null, { timeout: 90000 });
  const res = await pg.textContent('#upRes');
  console.log('upload:', res, (Date.now() - t0) + ' ms');
  ok(/ГОТОВО/.test(res) && /CRC совпал/.test(res), 'upload ok incl. read-back verify');
  const fl = await pg.evaluate(() => { const t = window.__tag; return { cur: t.cur, mask: t.cfg.mask, dropped: t.dropped, crcFlash: (() => { const o = 3 * 65536; return Array.from(t.flash.subarray(o, o + 46080)).length; })() }; });
  ok(fl.dropped, 'chunk loss was exercised (retransmission path)');
  ok(fl.cur === 3, 'slot 3 shown after upload');
  ok((fl.mask & 8) !== 0, 'slot 3 added to playlist mask');
  const fcrc = await pg.evaluate(() => { const t = window.__tag, o = 3 * 65536, b = t.flash.subarray(o, o + 46080); return window.__gs.crc32(b); });
  ok(fcrc === want, 'flash content equals frame sent');
  await pg.waitForTimeout(800);

  // --- slots tab
  await pg.click('[data-t=slots]'); await pg.waitForTimeout(400);
  const slotTxt = await pg.textContent('#slotGrid');
  ok(slotTxt.includes('совпадает с запомненным'), 'slot 3 matches stored picture');
  ok(await pg.evaluate(() => window.__gs.LIB.mem.has(3)), 'library holds slot 3');
  await pg.screenshot({ path: path.join(OUT, 'shot_slots.png') });

  // --- settings
  await pg.click('[data-t=cfg]');
  await pg.selectOption('#cInt', '5'); await pg.selectOption('#cMode', '1'); await pg.selectOption('#cTx', '-10'); await pg.fill('#cTemp', '24');
  await pg.click('#bCfg'); await pg.waitForTimeout(600);
  const cfg = await pg.evaluate(() => window.__tag.cfg);
  ok(cfg.iv === 5 && cfg.mode === 1 && cfg.tx === -10 && cfg.temp === 24 && cfg.en === 1 && (cfg.mask & 8), 'settings reached the tag: ' + JSON.stringify(cfg));
  await pg.screenshot({ path: path.join(OUT, 'shot_cfg.png') });

  // --- tag tab, hw
  await pg.click('[data-t=tag]');
  await pg.click('#bHw'); await pg.waitForTimeout(500);
  const hw = await pg.textContent('#hwKv');
  ok(hw.includes('1.0') && hw.includes('ef 40 13') && hw.includes('сторожевой'), 'hw info rendered');
  ok(await pg.evaluate(() => document.querySelector('#hwKv .blur') !== null), 'unique id is blurred by default');
  await pg.click('.fill[data-c="3"]'); await pg.waitForTimeout(300);
  ok(await pg.evaluate(() => window.__tag.fills[0] === 3), 'fill command sent');
  await pg.click('#bReboot'); await pg.waitForTimeout(300);
  ok(await pg.evaluate(() => window.__tag.rebooted === 1), 'reboot command sent');
  await pg.screenshot({ path: path.join(OUT, 'shot_tag.png') });

  // --- panel diagnostics (dead panel, then healthy)
  await pg.click('[data-t=tools]');
  await pg.evaluate(() => { window.__tag.dead = true; });
  await pg.click('#bPdiag'); await pg.waitForTimeout(700);
  let dg = await pg.textContent('#pdiagOut');
  ok(dg.includes('0x0000') && dg.includes('свободна') && dg.includes('Сырые значения'), 'diagnostics report: dead panel explained');
  await pg.evaluate(() => { window.__tag.dead = false; });
  await pg.click('#bPdiag'); await pg.waitForTimeout(700);
  dg = await pg.textContent('#pdiagOut');
  ok(dg.includes('Панель отвечает'), 'diagnostics report: healthy panel');
  // --- clock test
  await pg.click('#bClk'); await pg.waitForTimeout(600);
  let ck = await pg.textContent('#clkOut');
  ok(ck.includes('Кварц 32,768 кГц есть и в норме') && ck.includes('LFRCO') && ck.includes('ULFRCO') && ck.includes('32768.66 Гц') && ck.includes('+20 ppm'), 'clock test: crystal present, frequency shown');
  ok(ck.includes('запустился за 403 мс'), 'clock test: start time shown');
  console.log(ck);
  await pg.evaluate(() => { window.__tag.xtal = false; });
  await pg.click('#bClk'); await pg.waitForTimeout(600);
  ck = await pg.textContent('#clkOut');
  ok(ck.includes('Кварца 32,768 кГц, судя по всему, нет') && ck.includes('не запустился'), 'clock test: no crystal explained');

  // --- pin monitor
  await pg.click('#bPm2'); await pg.waitForTimeout(300);
  ok((await pg.$$eval('.pmc', e => e.length)) === 26, 'pin monitor: 26 cells');
  ok((await pg.evaluate(() => window.__tag.pmLen)) === 2, 'pin monitor: 2-byte command while all pins are selected (works on firmware 1.1)');
  ok((await pg.$$eval('.pmc.own', e => e.length)) === 14, 'pin monitor: 11 own pins + 3 SWD marked');
  ok((await pg.$$eval('.pmc.hi', e => e.length)) === 12, 'pin monitor: 12 watched pins high with pull-ups');
  await pg.evaluate(() => { const m = window.__tag.pm, b = 1 << 24; m.level &= ~b; m.lo |= b; m.edges = 1; m.last = 24; window.__tag.pmPush(); });
  await pg.waitForTimeout(300);
  ok((await pg.$$eval('.pmc.chg', e => e.map(x => x.textContent))).join('|').includes('PD2'), 'pin monitor: PD2 highlighted as changed');
  let pn = await pg.textContent('#pmNote');
  ok(pn.includes('только с подтяжкой вверх: PD2'), 'pin monitor: open-drain candidate named');
  ok((await pg.textContent('#pmStat')).includes('PD2'), 'pin monitor: last changed pin in the status line');
  await pg.click('#bPm1'); await pg.waitForTimeout(300);
  pn = await pg.textContent('#pmNote');
  ok(pn.includes('плавающие входы: менялись никакие') && pn.includes('подтяжка вверх: менялись PD2'), 'pin monitor: history kept per mode');
  await pg.check('#pmSwd'); await pg.waitForTimeout(300);
  ok((await pg.$$eval('.pmc.own', e => e.length)) === 11 && (await pg.$$eval('.pmc', e => e.length)) === 26, 'pin monitor: SWD pins watched when asked');
  await pg.uncheck('#pmSwd'); await pg.click('#bPmStop'); await pg.waitForTimeout(300);
  ok((await pg.textContent('#pmStat')).includes('выключен'), 'pin monitor: stopped');
  await pg.click('#bPmClr'); await pg.waitForTimeout(300);
  ok(!(await pg.textContent('#pmNote')).includes('менялись PD2'), 'pin monitor: history cleared');
  // pin selection: click a cell to leave that pin out
  await pg.evaluate(() => { window.__tag.old11 = true; });
  await pg.click('.pmc[data-i="25"]'); await pg.waitForTimeout(100); await pg.click('#bPm2'); await pg.waitForTimeout(300);
  ok((await pg.textContent('#pmStat')).includes('отклонила'), 'pin monitor: old firmware refusal is explained');
  await pg.evaluate(() => { window.__tag.old11 = false; });
  await pg.click('.pmc[data-i="25"]'); await pg.waitForTimeout(100);
  await pg.click('#bPmStop'); await pg.waitForTimeout(200);
  ok((await pg.$$eval('.pmc[data-i]', e => e.length)) === 12, 'pin monitor: 12 pins can be picked');
  await pg.click('.pmc[data-i="25"]'); await pg.waitForTimeout(200);
  ok((await pg.textContent('.pmc[data-i="25"]')).includes('исключён'), 'pin monitor: PD3 left out');
  await pg.click('#bPm2'); await pg.waitForTimeout(300);
  ok((await pg.evaluate(() => window.__tag.pmLen)) === 6 && (await pg.evaluate(() => window.__tag.pmMask)) === (0x3FFFFFF & ~(1 << 25)), 'pin monitor: mask sent with the mode');
  ok((await pg.$$eval('.pmc.hi', e => e.length)) === 11, 'pin monitor: 11 pins watched');
  await pg.click('.pmc[data-i="25"]'); await pg.waitForTimeout(300);
  ok((await pg.$$eval('.pmc.hi', e => e.length)) === 12, 'pin monitor: PD3 back in, monitor restarted');
  await pg.click('#bPmStop'); await pg.waitForTimeout(200);

  // --- I2C scanner
  ok((await pg.$$eval('#scPins input', e => e.length)) === 12 && (await pg.$$eval('#scPins input:checked', e => e.map(x => x.parentElement.textContent.trim()))).join() === 'PC5,PD2,PD3', 'scanner: 12 pins, PC5/PD2/PD3 chosen by default');
  await pg.selectOption('#scOpt', '1'); await pg.click('#bScan'); await pg.waitForTimeout(500);
  const sa = await pg.evaluate(() => window.__tag.scanArgs);
  ok(sa && sa.length === 7 && sa[1] === 2 && sa[2] === 1 && ((sa[3] | sa[4] << 8 | sa[5] << 16 | sa[6] << 24) >>> 0) === (((1 << 19) | (1 << 24) | (1 << 25)) >>> 0), 'scanner: command carries option and mask ' + JSON.stringify(sa));
  let so = await pg.textContent('#scOut');
  ok(so.includes('SDA=PD2, SCL=PD3, адрес 0x55') && so.includes('NTAG I²C') && so.includes('залипших 4') && so.includes('PD2, PD3'), 'scanner: device and hint shown');
  await pg.evaluate(() => { window.__tag.scanHit = false; });
  await pg.click('#bScan'); await pg.waitForTimeout(500);
  so = await pg.textContent('#scOut');
  ok(so.includes('Линии поднимаются, но на адреса'), 'scanner: nothing found explained');
  await pg.evaluate(() => { window.__tag.scanOk = false; });
  await pg.click('#bScan'); await pg.waitForTimeout(400);
  ok((await pg.textContent('#scOut')).includes('не приняла команду'), 'scanner: refusal explained');
  await pg.evaluate(() => { window.__tag.scanOk = true; });
  await pg.click('#bScanStop'); await pg.waitForTimeout(300);
  ok((await pg.textContent('#scOut')).includes('прервано'), 'scanner: abort shown');

  // --- tools: flash sector select has 0..14
  const secs = await pg.evaluate(() => document.querySelector('#sec').options.length);
  ok(secs === 15, 'flash test sectors 0..14');

  // --- mobile layout smoke
  await pg.setViewportSize({ width: 390, height: 800 }); await pg.click('[data-t=edit]'); await pg.waitForTimeout(300);
  const sw = await pg.evaluate(() => document.documentElement.scrollWidth);
  ok(sw <= 392, 'no horizontal scroll at 390px (' + sw + ')');
  await pg.screenshot({ path: path.join(OUT, 'shot_mobile.png') });

  ok(errs.length === 0, 'no console/page errors ' + JSON.stringify(errs));
  await br.close();
  console.log(fails.length ? 'FAILED: ' + fails.length : 'ALL OK');
  process.exit(fails.length ? 1 : 0);
})();
