// Service worker: lets Chrome install iGoWin as an app. Everything comes
// from the local server; only when the server is not running is a cached
// page shown that explains how to start it.
const CACHE = 'igowin-v2';

self.addEventListener('install', (event) => {
    event.waitUntil(caches.open(CACHE).then(cache => cache.add('offline.html')));
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', (event) => {
    if (event.request.mode !== 'navigate') return;
    event.respondWith(
        fetch(event.request).catch(() => caches.match('offline.html'))
    );
});
