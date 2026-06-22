#pragma once

// Process-wide cover/art ImageCache, shared by every widget that draws cover
// thumbnails (HEMUSIC-31). The cache owns a worker thread pool, so it must NOT
// live in a function-local static: tearing one down at DLL-unload time joins
// worker threads under the loader lock and races the d2d/WIC factory statics'
// destruction order. Instead the component's initquit owns the lifecycle --
// initCoverCache() on on_init, shutdownCoverCache() (explicit stop+join, before
// DLL detach) on on_quit.

namespace hemusic::ui {

class ImageCache;

// Construct the shared cache with the default WinHTTP cover fetcher. Call once
// from initquit::on_init (fb2k main thread). Idempotent.
void initCoverCache();

// Stop + join the cache's workers and destroy it. Call from initquit::on_quit
// (fb2k main thread), before the DLL detaches. Idempotent.
void shutdownCoverCache();

// The shared cache, or nullptr before initCoverCache() / after
// shutdownCoverCache(). Callers (paint) treat null as "no covers yet" and draw
// the placeholder box.
ImageCache* coverCache();

}  // namespace hemusic::ui
