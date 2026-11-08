/* Gecko Slide · SES V300: service worker (офлайн-режим). Репозиторий steanlab/SESoid, файл docs/sw.js.
   Страница (index.html) берётся сначала из сети, при отсутствии сети — из кэша, поэтому обновления приходят сразу, как только есть интернет.
   Остальное (иконки, манифест) — из кэша с фоновым обновлением. Для сброса кэша поменяйте CACHE. */
const CACHE = 'gecko-slide-v1';
const ASSETS = ['./', 'index.html', 'manifest.webmanifest', 'icons/icon.svg', 'icons/icon-192.png', 'icons/icon-512.png', 'icons/icon-maskable-512.png', 'icons/apple-touch-icon.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const r = e.request, u = new URL(r.url);
  if (r.method !== 'GET' || u.origin !== location.origin) return;
  const page = r.mode === 'navigate' || u.pathname.endsWith('/') || u.pathname.endsWith('/index.html');
  if (page) {
    e.respondWith(fetch(r).then(res => { if (res.ok) { const cp = res.clone(); caches.open(CACHE).then(c => c.put('index.html', cp)); } return res; })
      .catch(() => caches.match('index.html').then(m => m || caches.match('./'))));
  } else {
    e.respondWith(caches.match(r).then(m => {
      const net = fetch(r).then(res => { if (res.ok) { const cp = res.clone(); caches.open(CACHE).then(c => c.put(r, cp)); } return res; }).catch(() => m);
      return m || net;
    }));
  }
});
