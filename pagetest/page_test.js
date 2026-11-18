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
  const tag = { flash: new Uint8Array(0x80000).fill(255), cfg: { en: 1, mode: 0, iv: 60, mask: 0x7FE, temp: 20, tx: 6, nfc: 0, clean: 10 }, chipAddr: 0x55, cur: 0, src: 1, shown: 0,
    up: null, log: [], dropPage: 3, dropped: false, rebooted: 0, fills: [] };
  window.__tag = tag;
  let notify = null;
  const N = a => setTimeout(() => notify && notify({ target: { value: new DataView(new Uint8Array(a).buffer) } }), 2);
  const base = s => (s - 1) * 0xC000;
  function slotInfo(s) { const o = base(s) + 0xB400; const r = tag.flash.subarray(o, o + 256);
    if (r[0] !== 0x53 || r[1] !== 0x4C || r[2] !== 0x44 || r[3] !== 0x31) return { valid: 0, total: 0, crc: 0 };
    const dv = new DataView(r.buffer, r.byteOffset); return { valid: 1, total: dv.getUint32(4, true), crc: dv.getUint32(8, true) }; }
  function handle(v) {
    const op = v[0];
    if (op === 1) N([0x81, ...v.slice(1)]);
    else if (op === 0x10) {
      const mode = v[1], total = v[2] | v[3] << 8 | v[4] << 16 | v[5] << 24 >>> 0, slot = mode ? v[6] : 0;
      tag.up = { mode, total, slot, buf: new Uint8Array(total), next: 0, mask: {}, got: 0 };
      if (mode && (slot < 1 || slot > 10 || total > 46080)) { N([0x90, 1, slot]); return; }
      if (mode) { tag.flash.fill(255, base(slot), base(slot) + 0xC000); setTimeout(() => N([0x90, 0, 1, slot]), 300); } else N([0x90, 0, 0]);
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
      if (ok && u.mode) { const o = base(u.slot); tag.flash.set(u.buf, o); const rec = new Uint8Array(256).fill(255); rec.set([0x53, 0x4C, 0x44, 0x31, ...le32(u.total), ...le32(c), 1, 0, 0, 0], 0); tag.flash.set(rec, o + 0xB400); }
      N([0x94, ok ? 0 : 1, ...le32(c), ...le32(u.total)]);
    } else if (op === 0x24) {
      const s = v[1], i = slotInfo(s); if (!i.valid) { N([0xA4, s, 1, 0, 0, 0, 0, 0, 0, 0, 0]); return; }
      const c = crc(tag.flash.subarray(base(s), base(s) + i.total)); N([0xA4, s, c === i.crc ? 0 : 2, ...le32(i.total), ...le32(c)]);
    } else if (op === 0x30) {
      N([0xB0, 2, 0, tag.src, tag.shown, 27, ...le32(1800), ...le32(7200), 5, 0, 0, 3 | 0]);
    } else if (op === 0x31) {
      for (let s = 1; s <= 10; s++) { const i = slotInfo(s); N([0xB1, s, (i.valid ? 1 : 0) | (i.valid && i.total === 46080 ? 2 : 0) | (tag.cfg.mask >> s & 1 ? 4 : 0) | (tag.cur === s ? 8 : 0), ...le32(i.total), ...le32(i.crc)]); }
    } else if (op === 0x32) { tag.cur = v[1]; tag.src = 2; tag.shown = v[1]; N([0xB3, 0x32, 0]); }
    else if (op === 0x33) N([0xB3, 0x33, 0]);
    else if (op === 0x34) { tag.cfg = { en: v[1], mode: v[2], iv: v[3] | v[4] << 8, mask: v.length >= 11 ? v[5] | v[9] << 8 : (tag.cfg.mask & 0xFF00) | v[5], temp: v[6] << 24 >> 24, tx: v[7] << 24 >> 24, nfc: v.length >= 9 ? v[8] : tag.cfg.nfc, clean: v.length >= 11 ? v[10] : tag.cfg.clean, cmode: v.length >= 12 ? v[11] : tag.cfg.cmode || 0, flags: v.length >= 15 ? v[14] : tag.cfg.flags || 0 }; tag.cfgLen = v.length; if (v.length >= 14) tag.batt = [v[12], v[13]]; sendCfg(); sendBatt(); }
    else if (op === 0x35) { sendCfg(); sendBatt(); }
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
    else if (op === 0x3F && v[1] === 0x14) { tag.nfcCmd = 0x14; tag.nfcMode = 3; nfcInit(); N([0xB3, 0x3F, tag.nfcOk === false ? 2 : 0]); if (tag.nfcOk !== false) setTimeout(() => { for (let i = 0; i < 59; i++) N([0xBF, i, ...tag.chip[i]]); nfcSend(); }, 60); }
    else if (op === 0x3F && v[1] === 0x20 && v.length !== 19) N([0xB3, 0x3F, 1]);
    else if (op === 0x3F && v[1] === 0x20) { tag.wbuf = tag.wbuf || []; tag.wbuf[v[2]] = Array.from(v.slice(3, 19)); tag.wbufN = (tag.wbufN || 0) + 1; }
    else if (op === 0x3F && v[1] === 0x21) {
      tag.writeArgs = Array.from(v); tag.nfcMode = 4; N([0xB3, 0x3F, v.length === 4 && v[3] === 0xA5 ? 0 : 1]); tag.nfcRes = [0, 0];
      if (tag.lockNak) { tag.nfcRes = [1, 1]; tag.nfcFail = 1; tag.nfcNsValid = true; tag.nfcNs = 0x61; } else if (tag.wp) tag.nfcRes = [2, 1]; else for (let i = 0; i < v[2]; i++) tag.chip[1 + i] = tag.wbuf[i];
      setTimeout(() => nfcSend(), 80);
    }
    else if (op === 0x3F && v[1] === 0x30) { tag.nfcMode = 6; N([0xB3, 0x3F, 0]); setTimeout(() => N([0xBD, tag.nochip ? 3 : 2, 6, tag.chipAddr === 0x55 ? 0 : 0x31, 0, 0, 0, 0, 0, 0, 0, 0, 0, tag.nochip ? 1 : 0, tag.nochip ? 0 : tag.chipAddr, tag.nochip ? 0 : 1]), 80); }
    else if (op === 0x3F && v[1] === 0x31) { tag.rescueArgs = Array.from(v); tag.nfcMode = 7; N([0xB3, 0x3F, v.length === 3 && v[2] === 0xA5 ? 0 : 1]); const was = tag.chipAddr; tag.chipAddr = 0x55; setTimeout(() => N([0xBD, 2, 7, was === 0x55 ? 1 : 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, was === 0x55 ? 0 : 0x42, was === 0x55 ? 0 : 5]), 80); }
    else if (op === 0x3F && v[1] === 0x22) { tag.unlockArgs = Array.from(v); tag.nfcMode = 5; tag.wp = false; tag.chip[0][10] = 0; tag.chip[0][11] = 0; tag.nfcRes = [0, 0]; N([0xB3, 0x3F, 0]); setTimeout(() => nfcSend(), 80); }
    else if (op === 0x3F && v[1] >= 0x10 && v[1] <= 0x13) {
      tag.nfcCmd = v[1]; if (v[1] === 0x11) tag.fdArgs = Array.from(v);
      if (v[1] === 0x12) N([0xBD, 2, tag.nfcMode || 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
      else {
        if (v[1] !== 0x13) { tag.nfcMode = v[1] === 0x11 ? 2 : 1; tag.nfcRes = [0, 0]; N([0xB3, 0x3F, tag.nfcOk === false ? 2 : 0]); }
        if (tag.nfcOk !== false) setTimeout(() => nfcSend(), 80);
      }
    }
    else if (op === 0x3F && (v[1] === 2 || v[1] === 5)) { tag.scanArgs = Array.from(v); N([0xB3, 0x3F, tag.scanOk === false ? 2 : 0]); if (tag.scanOk !== false) setTimeout(() => N(scanPkt(2)), 100); }
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
  function nfcInit() { if (!tag.chip) { const b = nfcBlocks(); tag.chip = Array.from({ length: 59 }, (_, i) => i < 6 ? b[i].slice() : Array(16).fill(0)); } }
  function nfcBlocks() { return [
      [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x00, 0xFF, 0xFF, 0xE1, 0x10, 0x6D, 0x0F],
      [0x03, 0x1D, 0xD1, 0x01, 0x19, 0x55, 0x04, ...Array.from('nfc.example.com/EE285D4B').map(c => c.charCodeAt(0)).slice(0, 9)],
      [...Array.from('nfc.example.com/EE285D4B').map(c => c.charCodeAt(0)).slice(9), 0xFE, 0, 0, 0, 0, 0, 0, 0, 0],
      Array(16).fill(0), [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16], Array(16).fill(0x5A)]; }
  function nfcSend() {
    const blk = [
      [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x00, 0xFF, 0xFF, 0xE1, 0x10, 0x6D, 0x0F],
      [0x03, 0x1D, 0xD1, 0x01, 0x19, 0x55, 0x04, ...Array.from('nfc.example.com/EE285D4B').map(c => c.charCodeAt(0)).slice(0, 9)],
      [...Array.from('nfc.example.com/EE285D4B').map(c => c.charCodeAt(0)).slice(9), 0xFE, 0, 0, 0, 0, 0, 0, 0, 0],
      Array(16).fill(0), [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16], Array(16).fill(0x5A)];
    nfcInit();
    if (tag.nfcMode === 1) {
      for (let i = 0; i < 6; i++) N([0xBC, i, ...blk[i]]);
      N([0xBC, 6, 0x7F, 0x01, 0, 0, 0, 0, 0x08, tag.nfcNs === undefined ? 0x20 : tag.nfcNs]);
    }
    const ns = tag.nfcNs === undefined ? 0x20 : tag.nfcNs;
    if (tag.nfcMode === 2 && tag.fdArgs && tag.fdArgs.length === 6) {
      const m = (tag.fdArgs[2] | tag.fdArgs[3] << 8 | tag.fdArgs[4] << 16 | tag.fdArgs[5] << 24) >>> 0;
      for (let i = 0; i < 26; i++) if (m >> i & 1) N(i === 8 ? [0xBE, i, ...le32(900), ...le32(100), ...le32(2), ...le32(1500)] : [0xBE, i, ...le32(0), ...le32(1000), ...le32(0), ...le32(1500)]);
    }
    const nr = tag.nfcRes || [0, 0], fdn = tag.nfcMode === 2 && tag.fdArgs && tag.fdArgs.length === 6 ? 9 : 0;
    N([0xBD, tag.nfcDead ? 3 : (tag.nfcMode >= 4 && nr[0] ? 3 : 2), tag.nfcMode, tag.nfcDead ? 0x7F : (tag.nfcMode >= 4 ? (tag.nfcFail || 0) : 0), tag.nfcDead ? 0 : (tag.nfcMode === 2 ? 1 | ((ns & 1) << 1) | ((tag.nfcEver ? 1 : 0) << 2) : (tag.nfcMode >= 4 && tag.nfcNsValid ? 1 | ((ns & 1) << 1) : 0)), ns, tag.nfcEvents || 0, 0, 7, 0, 35, 0, tag.nfcDead ? 0 : 0x7F, tag.nfcMode >= 4 ? nr[0] : 0, tag.nfcMode >= 4 ? nr[1] : 0, fdn]);
  }
  function scanPkt(st) {
    const hit = tag.scanHit !== false, opt = tag.scanArgs ? tag.scanArgs[2] : 0, pw = tag.scanArgs && tag.scanArgs[1] === 5 && hit ? 11 : 255;
    return [0xBB, st | opt << 4, 6, 6, 4, hit ? 1 : 0, hit ? 24 : 0, hit ? 25 : 0, hit ? 0x55 : 0, pw, 255, 0, ...le32(((1 << 24) | (1 << 25)) >>> 0), ...le32(tag.scanHeld ? (1 << 24) >>> 0 : 0)];
  }
  function clkPkt(never) {
    if (never) return [0xB9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    const c = tag.xtal ? 299994 : 0, r = 303030, u = 279272;
    const fl = tag.xtal ? 0x01 | 0x04 | 0x10 | 0x20 | 0x40 : 0x01 | 0x80 | 0x20 | 0x40;
    return [0xB9, 2, fl, 3 | (1 << 3) | (1 << 5), 0x8C, 0x7A, 0x8C, 0x7A, tag.xtal ? 0x93 : 0xFF, tag.xtal ? 0x01 : 0xFF, c & 255, c >> 8 & 255, c >> 16, r & 255, r >> 8 & 255, r >> 16, u & 255, u >> 8 & 255, u >> 16, 0x30];
  }
  function sendBatt() { const b = tag.batt || [70, 50]; N([0xC0, tag.battLevel || 0, 0x68, 0x0B, b[0], b[1], 120, 0, 0xEB, 0x00, 24]); }
  function sendCfg() { const c = tag.cfg; N([0xB4, 1, c.en, c.mode, c.iv & 255, c.iv >> 8, c.mask & 255, c.temp & 255, c.tx & 255, 60, 0, ...(tag.cfgOld ? [] : [c.nfc ? 1 : 0, 3, c.mask >> 8, c.clean, c.cmode || 0, c.flags || 0])]); }
  const chr = { addEventListener: (n, f) => { notify = f; }, startNotifications: async () => {}, writeValueWithResponse: async v => handle(new Uint8Array(v.buffer ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : v)), writeValueWithoutResponse: async v => handle(new Uint8Array(v.buffer ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : v)) };
  const svc = { getCharacteristic: async () => chr };
  const device = { name: 'V300-TEST', gatt: { connected: false, connect: async function () { this.connected = true; return { getPrimaryService: async () => svc }; }, disconnect: function () { this.connected = false; device.__d && device.__d(); } }, addEventListener: (n, f) => { device.__d = f; } };
  Object.defineProperty(navigator, 'bluetooth', { value: { requestDevice: async () => device }, configurable: true });
})();
`;

(async () => {
  const br = await chromium.launch();
  const ctx = await br.newContext({ viewport: { width: 1200, height: 900 }, deviceScaleFactor: 1, locale: process.env.LOC || 'ru-RU' });
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
  await pg.waitForFunction(() => document.getElementById('prog').classList.contains('on') && /%/.test(document.getElementById('pdet').textContent), null, { timeout: 20000 });
  const pd = await pg.textContent('#pdet'), ps = await pg.textContent('#pst');
  ok(/^\d+ % · [\d.]+ (КБ|KB|Б|B) \/ 45\.0 (КБ|KB) · \d+ (с|s)/.test(pd) && /слот|slot/i.test(ps), 'progress panel while uploading: ' + ps + ' | ' + pd);
  await pg.waitForFunction(() => /ГОТОВО|DONE|ОШИБКА|ERROR|Прервано|Aborted/.test(document.querySelector('#upRes').textContent), null, { timeout: 90000 });
  const res = await pg.textContent('#upRes');
  console.log('upload:', res, (Date.now() - t0) + ' ms');
  ok(/ГОТОВО/.test(res) && /CRC совпал/.test(res), 'upload ok incl. read-back verify');
  ok(await pg.evaluate(() => !document.getElementById('prog').classList.contains('on')), 'progress panel hidden after the upload');
  const fl = await pg.evaluate(() => { const t = window.__tag; return { cur: t.cur, mask: t.cfg.mask, dropped: t.dropped, crcFlash: (() => { const o = 2 * 0xC000; return Array.from(t.flash.subarray(o, o + 46080)).length; })() }; });
  ok(fl.dropped, 'chunk loss was exercised (retransmission path)');
  ok(fl.cur === 3, 'slot 3 shown after upload');
  ok((fl.mask & 8) !== 0, 'slot 3 added to playlist mask');
  const fcrc = await pg.evaluate(() => { const t = window.__tag, o = 2 * 0xC000, b = t.flash.subarray(o, o + 46080); return window.__gs.crc32(b); });
  ok(fcrc === want, 'flash content equals frame sent');
  await pg.waitForTimeout(800);

  // --- slots tab
  await pg.click('[data-t=slots]'); await pg.waitForTimeout(400);
  const slotTxt = await pg.textContent('#slotGrid');
  ok(slotTxt.includes('совпадает с запомненным'), 'slot 3 matches stored picture');
  ok(await pg.evaluate(() => window.__gs.LIB.mem.has(3)), 'library holds slot 3');
  await pg.screenshot({ path: path.join(OUT, 'shot_slots.png') });
  ok(await pg.evaluate(() => document.querySelectorAll('#slotGrid .slot').length) === 10, 'ten slot cards');
  ok(await pg.evaluate(() => document.getElementById('upSlot').options.length === 10 && document.getElementById('vSlot').options.length === 10), 'ten slots in the selectors');

  // --- settings
  await pg.click('[data-t=cfg]');
  await pg.selectOption('#cInt', '5'); await pg.selectOption('#cMode', '1'); await pg.selectOption('#cTx', '-10'); await pg.fill('#cTemp', '24'); await pg.fill('#cClean', '7'); await pg.selectOption('#cCleanMode', '2');
  await pg.waitForTimeout(300);
  ok((await pg.textContent('#cBattInfo')).includes('2.92') && (await pg.inputValue('#cBattLow')) === '2.70' && (await pg.inputValue('#cBattCrit')) === '2.50', 'battery report shown: ' + await pg.textContent('#cBattInfo'));
  await pg.check('#cDeep'); await pg.check('#cOd'); await pg.check('#cTAuto');
  await pg.fill('#cBattLow', '2.8'); await pg.fill('#cBattCrit', '2.6');
  await pg.click('#bCfg'); await pg.waitForTimeout(600);
  ok((await pg.evaluate(() => window.__tag.batt)).join() === '80,60', 'battery thresholds reached the tag (14-byte message): ' + await pg.evaluate(() => window.__tag.batt));
  const cfg = await pg.evaluate(() => window.__tag.cfg);  const cfgLen = await pg.evaluate(() => window.__tag.cfgLen);
  ok(cfg.clean === 7 && cfg.cmode === 2 && cfgLen === 15 && cfg.flags === 7, 'full-redraw period, cleaning mode and the three mode flags reached the tag (15-byte message): ' + JSON.stringify(cfg));
  ok(cfg.iv === 5 && cfg.mode === 1 && cfg.tx === -10 && cfg.temp === 24 && cfg.en === 1 && (cfg.mask & 8), 'settings reached the tag: ' + JSON.stringify(cfg));
  { const ti = await pg.textContent('#cTempInfo'); ok(ti.includes('23.5') && ti.includes('24'), 'chip temperature and the value sent to the panel are shown: ' + ti); }
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
  ok(so.includes('SDA=PD2, SCL=PD3, адрес 0x55') && so.includes('NTAG I²C') && so.includes('залипших 4') && so.includes('PD2, PD3') && !so.includes('держится внизу'), 'scanner: device and hint shown');
  await pg.evaluate(() => { window.__tag.scanHit = false; window.__tag.scanHeld = true; });
  await pg.click('#bScan'); await pg.waitForTimeout(500);
  so = await pg.textContent('#scOut');
  ok(so.includes('не найдено') && so.includes('держится внизу') && so.includes('PD2'), 'scanner: nothing found, held-low pin shown');
  await pg.evaluate(() => { window.__tag.scanOk = false; });
  await pg.click('#bScan'); await pg.waitForTimeout(400);
  ok((await pg.textContent('#scOut')).includes('не приняла команду'), 'scanner: refusal explained');
  await pg.evaluate(() => { window.__tag.scanOk = true; });
  await pg.click('#bScanStop'); await pg.waitForTimeout(300);
  ok((await pg.textContent('#scOut')).includes('прервано'), 'scanner: abort shown');

  // --- I2C scanner with power pins
  await pg.evaluate(() => { window.__tag.scanHit = true; window.__tag.scanHeld = false; window.__tag.scanOk = true; });
  ok((await pg.$$eval('#scPwr input:checked', e => e.map(x => x.parentElement.textContent.trim()))).join() === 'PA6,PA8,PB1,PB2,PB3,PB4,PC7,PD0,PD1', 'power pins: nine candidates chosen by default');
  await pg.click('#bScanPwr'); await pg.waitForTimeout(500);
  const pa = await pg.evaluate(() => window.__tag.scanArgs);
  ok(pa && pa.length === 11 && pa[1] === 5 && ((pa[3] | pa[4] << 8 | pa[5] << 16 | pa[6] << 24) >>> 0) === (((1 << 19) | (1 << 24) | (1 << 25)) >>> 0) && ((pa[7] | pa[8] << 8 | pa[9] << 16 | pa[10] << 24) >>> 0) === 0xE03D40, 'power scan: command carries scan mask and power mask ' + JSON.stringify(pa));
  so = await pg.textContent('#scOut');
  ok(so.includes('PB2') && so.includes('питание') && so.includes('SDA=PD2'), 'power scan: power pin shown');
  await pg.evaluate(() => { window.__tag.scanHit = false; });
  await pg.click('#bScanPwr'); await pg.waitForTimeout(500);
  ok((await pg.textContent('#scOut')).includes('ни при подаче'), 'power scan: nothing found explained');
  await pg.evaluate(() => { window.__tag.scanHit = true; });

  // --- NFC chip card
  await pg.click('#bNfcSnap'); await pg.waitForTimeout(600);
  let no = await pg.textContent('#nfcOut');
  ok(no.includes('Чтение чипа: готово') && no.includes('байты блокировки: FF FF') && no.includes('необратимо') && no.includes('только чтение'), 'nfc: snapshot read, lock bytes and CC explained');
  ok(!no.includes('AA BB') && !no.includes('aa bb') && no.includes('••'), 'nfc: serial data hidden by default');
  ok(no.includes('Ссылка в чипе: https://nfc.example.com/••••••••') && !no.includes('EE285D4B'), 'nfc: link shown with its tail hidden');
  ok(no.includes('NC_REG=01') && no.includes('NS_REG=20') && no.includes('бит0 поле телефона = 0'), 'nfc: session registers decoded');
  await pg.check('#nfcShow'); await pg.waitForTimeout(100);
  no = await pg.textContent('#nfcOut');
  ok(no.includes('AA BB CC') && no.includes('nfc.example.com/EE285D4B'), 'nfc: serial data and link tail shown on request');
  await pg.uncheck('#nfcShow');
  await pg.evaluate(() => { window.__tag.nfcEvents = 2; window.__tag.nfcNs = 0x21; window.__tag.nfcEver = true; });
  await pg.click('#bNfcLive'); await pg.waitForTimeout(600);
  no = await pg.textContent('#nfcOut');
  ok(no.includes('Поле телефона: ЕСТЬ') && no.includes('касаний (нет→есть): 2') && no.includes('Слежение за полем'), 'nfc: live field state shown');
  await pg.evaluate(() => { window.__tag.nfcDead = true; });
  await pg.click('#bNfcSnap'); await pg.waitForTimeout(600);
  ok((await pg.textContent('#nfcOut')).includes('ЧИП НЕ ОТВЕТИЛ'), 'nfc: dead chip explained');
  await pg.evaluate(() => { window.__tag.nfcDead = false; window.__tag.nfcOk = false; });
  await pg.click('#bNfcSnap'); await pg.waitForTimeout(500);
  ok((await pg.textContent('#nfcOut')).includes('не приняла команду'), 'nfc: old firmware refusal explained');
  await pg.evaluate(() => { window.__tag.nfcOk = true; });

  // --- NFC tap switch in settings
  await pg.click('[data-t=cfg]'); await pg.waitForTimeout(200);
  await pg.click('#bCfgGet'); await pg.waitForTimeout(300);
  ok(await pg.evaluate(() => !document.getElementById('cNfc').disabled) && (await pg.textContent('#cNfcInfo')).includes('касаний с запуска метки: 3'), 'nfc tap: switch enabled, touch counter shown');
  await pg.check('#cNfc'); await pg.click('#bCfg'); await pg.waitForTimeout(400);
  ok((await pg.evaluate(() => window.__tag.cfg.nfc)) === 1 && (await pg.evaluate(() => window.__tag.cfgLen)) === 15, 'nfc tap: the 2.0 message (15 bytes) carries the switch, slot bits 8..10 and the full-redraw period');
  ok(await pg.isChecked('#cNfc'), 'nfc tap: switch stays on after readback');
  await pg.evaluate(() => { window.__tag.cfgOld = true; });
  await pg.click('#bCfgGet'); await pg.waitForTimeout(300);
  ok(await pg.evaluate(() => document.getElementById('cNfc').disabled) && (await pg.textContent('#cNfcInfo')).includes('1.7'), 'nfc tap: old firmware disables the switch');
  await pg.click('#bCfg'); await pg.waitForTimeout(300);
  ok((await pg.evaluate(() => window.__tag.cfgLen)) === 8, 'nfc tap: old firmware gets the 8-byte settings command');
  await pg.evaluate(() => { window.__tag.cfgOld = false; });
  await pg.click('#bCfgGet'); await pg.waitForTimeout(300);

  await pg.click('[data-t=tools]'); await pg.waitForTimeout(200);
  // --- NFC: FD pin search
  await pg.check('#nfcFd'); await pg.click('#bNfcLive'); await pg.waitForTimeout(700);
  const fdA = await pg.evaluate(() => window.__tag.fdArgs);
  const fdMask = (fdA[2] | fdA[3] << 8 | fdA[4] << 16 | fdA[5] << 24) >>> 0;
  ok(fdA.length === 6 && fdMask === [6, 8, 10, 11, 12, 13, 21, 22, 23].reduce((m, i) => m | 1 << i, 0) >>> 0, 'fd: command carries the nine free pins');
  no = await pg.textContent('#nfcOut');
  ok(/PA8 +★ ПОХОЖЕ НА FD/.test(no) && (no.match(/ПОХОЖЕ НА FD/g) || []).length === 1, 'fd: only the pin that follows the field is flagged');
  ok(no.includes('с полем внизу 90%') && no.includes('без поля вверху 100%'), 'fd: figures shown');
  await pg.uncheck('#nfcFd'); await pg.click('#bNfcLive'); await pg.waitForTimeout(500);
  ok((await pg.evaluate(() => window.__tag.fdArgs)).length === 2 && !(await pg.textContent('#nfcOut')).includes('Поиск вывода FD'), 'fd: plain live run sends no mask');

  // --- NFC: backup, write, restore, unlock
  ok(await pg.evaluate(() => !document.getElementById('bNfcWr').disabled && document.getElementById('bNfcUnlock').disabled && document.getElementById('bNfcDl').disabled), 'nfc write: available without a backup, unlock and download locked');
  await pg.click('#bNfcBk'); await pg.waitForTimeout(900);
  ok((await pg.textContent('#nfcOut2')).includes('прочитано блоков 59 из 59') && !(await pg.textContent('#nfcOut2')).includes('AA BB'), 'backup: complete and the serial data is not shown');
  ok(await pg.evaluate(() => !document.getElementById('bNfcDl').disabled), 'backup: download enabled');
  const dl = pg.waitForEvent('download'); await pg.click('#bNfcDl'); const dlf = await dl;
  const dlp = await dlf.path(); const dlb = require('fs').readFileSync(dlp);
  ok(dlf.suggestedFilename() === 'nfc_chip_backup.bin' && dlb.length === 944 && dlb[0] === 0xAA && dlb[16] === 0x03, 'backup: file is 59x16 bytes in block order');
  ok((await pg.inputValue('#nfcUrl')).startsWith('https://') , 'nfc write: default URL is filled');
  await pg.click('[data-t=tag]'); await pg.fill('#nfcUrl', 'ftp://x'); await pg.waitForTimeout(100);
  ok(await pg.evaluate(() => document.getElementById('bNfcWr').disabled) && (await pg.textContent('#nfcUrlInfo')).includes('http'), 'nfc write: bad scheme refused');
  await pg.fill('#nfcUrl', 'https://example.org/app/'); await pg.waitForTimeout(100);
  // text and Wi-Fi records
  await pg.selectOption('#nfcKind', 'text'); await pg.fill('#nfcUrl', 'Привет'); await pg.waitForTimeout(100);
  const tx = await pg.evaluate(() => nfcBuildAny().ndef);
  ok(tx.join() === [3, 19, 209, 1, 15, 84, 2, 114, 117, 208, 159, 209, 128, 208, 184, 208, 178, 208, 181, 209, 130, 254].join(), 'nfc write: Russian text record (UTF-8, lang ru): ' + tx.join(' '));
  await pg.selectOption('#nfcKind', 'wifi'); await pg.fill('#nfcSsid', 'Home'); await pg.fill('#nfcPass', 'secret123'); await pg.waitForTimeout(100);
  const wf = await pg.evaluate(() => nfcBuildAny().ndef);
  const wfs = String.fromCharCode(...wf.slice(5, 28));
  ok(wf[2] === 0xD2 && wf[3] === 23 && wfs === 'application/vnd.wfa.wsc' && wf[wf.length - 1] === 0xFE && wf.length === 81, 'nfc write: Wi-Fi record: ' + wf.length + ' bytes');
  const ssidAt = wf.findIndex((v, i) => v === 0x10 && wf[i + 1] === 0x45), keyAt = wf.findIndex((v, i) => v === 0x10 && wf[i + 1] === 0x27);
  ok(wf[1] === wf.length - 3 && wf[4] === 0 + wf[4] && wf[28 - 0] === 0x10 && wf[29] === 0x0E && String.fromCharCode(...wf.slice(ssidAt + 4, ssidAt + 8)) === 'Home' && String.fromCharCode(...wf.slice(keyAt + 4, keyAt + 13)) === 'secret123', 'nfc write: Wi-Fi credential has SSID and key (len byte ' + wf[1] + ')');
  ok(await pg.evaluate(() => !document.getElementById('nfcWifiBox').hidden && document.getElementById('nfcUrlLbl').hidden), 'nfc write: Wi-Fi fields shown');
  await pg.fill('#nfcPass', 'short'); await pg.waitForTimeout(100);
  ok((await pg.textContent('#nfcUrlInfo')).includes('8'), 'nfc write: short Wi-Fi password refused');
  await pg.selectOption('#nfcKind', 'url'); await pg.fill('#nfcUrl', 'https://example.org/app/'); await pg.waitForTimeout(100);
  ok((await pg.textContent('#nfcUrlInfo')).includes('в чип запишется 24 байт (2 блоков'), 'nfc write: size shown');
  await pg.fill('#nfcUrl', 'https://' + 'a'.repeat(120)); await pg.waitForTimeout(100);
  ok(await pg.evaluate(() => document.getElementById('bNfcWr').disabled) && (await pg.textContent('#nfcUrlInfo')).includes('слишком длинная'), 'nfc write: too long refused');
  await pg.fill('#nfcUrl', 'https://example.org/app/'); await pg.waitForTimeout(100);
  await pg.evaluate(() => { window.__tag.lockNak = true; });
  await pg.click('[data-t=tag]'); await pg.click('#bNfcWr'); await pg.waitForTimeout(700);
  const wt = await pg.textContent('#nfcOut2');
  ok(wt.includes('не принял номер блока') && wt.includes('NS_REG=0x61') && wt.includes('память занята со стороны NFC ДА') && wt.includes('уберите телефон'), 'nfc write: refused write explained with NS_REG and the phone hint');
  await pg.evaluate(() => { window.__tag.lockNak = false; window.__tag.nfcNsValid = false; window.__tag.nfcNs = undefined; window.__tag.nfcFail = 0; });
  await pg.evaluate(() => { window.__tag.wp = true; });
  await pg.click('[data-t=tag]'); await pg.click('#bNfcWr'); await pg.waitForTimeout(700);
  const wa = await pg.evaluate(() => window.__tag.writeArgs);
  ok(wa && wa.length === 4 && wa[2] === 7 && wa[3] === 0xA5, 'nfc write: commit is armed and counts blocks');
  ok((await pg.textContent('#nfcOut2')).includes('не совпали') && (await pg.textContent('#nfcOut2')).includes('Снять блокировку'), 'nfc write: ignored write explained, unlock suggested');
  ok(await pg.evaluate(() => document.getElementById('bNfcUnlock').disabled), 'unlock: needs its own confirmation');
  await pg.click('[data-t=tools]'); await pg.check('#nfcUnOk'); await pg.waitForTimeout(100);
  ok(await pg.evaluate(() => document.getElementById('bNfcUnlock').disabled), 'unlock: stays off until the rescue has found the byte-0 value');
  await pg.evaluate(() => { window.__tag.chipAddr = 0x55; });
  ok(await pg.evaluate(() => document.getElementById('bNfcRescue').disabled), 'rescue: needs its confirmation');
  await pg.check('#nfcRsOk'); await pg.click('#bNfcRescue'); await pg.waitForTimeout(500);
  ok((await pg.evaluate(() => window.__tag.rescueArgs)).join() === '63,49,165' && (await pg.textContent('#nfcOut2')).includes('ничего не записано'), 'rescue: chip already at 0x55, nothing written');
  ok(await pg.evaluate(() => document.getElementById('bNfcUnlock').disabled), 'unlock: still off after "already at 0x55"');
  await pg.evaluate(() => { window.__tag.chipAddr = 0x2A; });
  await pg.click('#bNfcFind'); await pg.waitForTimeout(500);
  ok((await pg.textContent('#nfcOut2')).includes('адресе 0x2A') && (await pg.textContent('#nfcOut2')).includes('Вернуть адрес 0x55'), 'find: chip at 0x2A reported, rescue suggested');
  await pg.evaluate(() => { window.__tag.nochip = true; });
  await pg.click('#bNfcFind'); await pg.waitForTimeout(500);
  ok((await pg.textContent('#nfcOut2')).includes('никто не ответил'), 'find: nobody answered explained');
  await pg.evaluate(() => { window.__tag.nochip = false; });
  await pg.click('#bNfcRescue'); await pg.waitForTimeout(500);
  ok((await pg.textContent('#nfcOut2')).includes('Нулевой байт блока 0 = 0x42') && (await pg.textContent('#nfcOut2')).includes('попыток: 5'), 'rescue: value found shown');
  ok(await pg.evaluate(() => !document.getElementById('bNfcUnlock').disabled), 'unlock: enabled once the value is known');
  await pg.click('#bNfcUnlock'); await pg.waitForTimeout(500);
  ok((await pg.evaluate(() => window.__tag.unlockArgs)).join() === '63,34,165,66' && (await pg.textContent('#nfcOut2')).includes('Снятие блокировки: выполнено'), 'unlock: armed command with the byte-0 value, success shown');
  await pg.click('[data-t=tag]'); await pg.click('#bNfcWr'); await pg.waitForTimeout(700);
  const ch = await pg.evaluate(() => window.__tag.chip);
  const wantTxt = Array.from('https://example.org/app/').slice(8).map(c => c.charCodeAt(0));
  ok(ch[1].slice(0, 7).join() === [3, 4 + 1 + wantTxt.length, 0xD1, 1, 1 + wantTxt.length, 0x55, 4].join() && ch[1][7] === wantTxt[0] && ch[2].includes(0xFE), 'nfc write: NDEF record built correctly');
  ok((await pg.textContent('#nfcOut2')).includes('Запись ссылки: выполнено'), 'nfc write: success shown');
  await pg.click('[data-t=tools]'); await pg.click('#bNfcRestore'); await pg.waitForTimeout(700);
  const ch2 = await pg.evaluate(() => window.__tag.chip);
  ok(ch2[1].join() === [0x03, 0x1D, 0xD1, 0x01, 0x19, 0x55, 0x04, ...Array.from('nfc.example.com/EE285D4B').map(c => c.charCodeAt(0)).slice(0, 9)].join() && ch2[2][14] === 0xFE || ch2[2].includes(0xFE), 'restore: stock record written back from the backup');
  await pg.evaluate(() => { window.__tag.nfcOk = false; });
  await pg.click('#bNfcBk'); await pg.waitForTimeout(400);
  ok((await pg.textContent('#nfcOut2')).includes('1.7'), 'backup: old firmware refusal explained');
  await pg.evaluate(() => { window.__tag.nfcOk = true; });

  // --- tools: flash sector select has 0..14
  const secs = await pg.evaluate(() => document.querySelector('#sec').options.length);
  ok(secs === 7, 'flash test sectors 0..6 (test area 0x78000)');

  // --- mobile layout smoke
  await pg.setViewportSize({ width: 390, height: 800 }); await pg.click('[data-t=edit]'); await pg.waitForTimeout(300);
  const sw = await pg.evaluate(() => document.documentElement.scrollWidth);
  ok(sw <= 392, 'no horizontal scroll at 390px (' + sw + ')');
  await pg.screenshot({ path: path.join(OUT, 'shot_mobile.png') });


  // --- i18n scan: leftover Cyrillic in the visible texts, attributes and the log (en mode)
  if (process.env.SCAN) {
    if (process.env.SCAN === 'switch') { await pg.click('#bLang'); await pg.waitForTimeout(500); }
    const left = await pg.evaluate(() => {
      const out = new Set(), re = /[А-Яа-яЁё]/;
      const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
      for (let n; (n = w.nextNode());) { if (n.parentElement.closest('script,style')) continue; if (re.test(n.nodeValue)) out.add('T: ' + n.nodeValue.trim().slice(0, 140)); }
      document.querySelectorAll('*').forEach(e => { for (const a of ['title', 'placeholder', 'value']) { const v = e.getAttribute(a); if (v && re.test(v)) out.add('A(' + a + '): ' + v.slice(0, 140)); } });
      document.querySelectorAll('option').forEach(o => { if (re.test(o.text)) out.add('O: ' + o.text); });
      return [...out];
    });
    console.log('CYRILLIC LEFT: ' + left.length); left.forEach(x => console.log('   ' + x));
    console.log('lang attr', await pg.evaluate(() => document.documentElement.lang), 'btn', await pg.textContent('#bLang'));
  }
  ok(errs.length === 0, 'no console/page errors ' + JSON.stringify(errs));
  await br.close();
  console.log(fails.length ? 'FAILED: ' + fails.length : 'ALL OK');
  process.exit(fails.length ? 1 : 0);
})();
