// matata YouTube / Vimeo overlay.
//
// Content scripts can't see page-context globals like ytInitialPlayerResponse.
// We bridge by asking the background (which has chrome.scripting) to run a
// MAIN-world extractor — that bypasses the page CSP that would otherwise
// block any inline <script> we tried to inject ourselves.

(() => {
"use strict";

console.log("[matata] yt-extract content script loaded on", location.href);

const isYouTube = /(?:^|\.)youtube\.com$/.test(location.hostname) || location.hostname === "music.youtube.com";
const isVimeo   = /(?:^|\.)vimeo\.com$/.test(location.hostname);

function siteKind() {
  if (isYouTube) return "youtube";
  if (isVimeo)   return "vimeo";
  return "";
}

function extractFromBackground() {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendMessage(
        { action: "extract-page-formats", site: siteKind() },
        (resp) => resolve(resp || null));
    } catch { resolve(null); }
  });
}

function youtubeWatchFallback() {
  // Last-ditch fallback when even the metadata extractor returns nothing.
  // Synthetic 2-option menu — matata-gui's yt-dlp will pick a sensible default.
  if (!isYouTube) return null;
  if (!/\/watch|\/shorts\/|\/playlist/.test(location.pathname + location.search)) return null;
  return {
    site:    "youtube",
    videoId: new URLSearchParams(location.search).get("v") || "",
    title:   document.title.replace(/\s*-\s*YouTube\s*$/, "") || "YouTube video",
    author:  "",
    duration:0,
    pageUrl: location.href,
    formats: [],
    pickerOptions: [
      { label: "Best video + audio", meta: "via yt-dlp", ytFormat: "bv*+ba/best",            container: "mp4" },
      { label: "Audio only (M4A)",   meta: "via yt-dlp", ytFormat: "bestaudio[ext=m4a]/bestaudio", container: "m4a", isAudio: true }
    ]
  };
}

// Build picker options (one row per available height + audio-only) from the
// raw streamingData formats list extracted from ytInitialPlayerResponse.
function buildYtPickerOptions(meta) {
  if (!meta || !Array.isArray(meta.formats) || meta.formats.length === 0) return [];

  // Collect distinct video heights and best audio bitrate seen.
  const heightInfo = new Map(); // height -> { fps, bestSize }
  let hasAudio = false;
  let bestAudioKbps = 0;

  for (const f of meta.formats) {
    const h = f.height || 0;
    if (f.isVideo && h > 0) {
      const cur = heightInfo.get(h) || { fps: f.fps || 0, bestSize: 0, codec: "" };
      if ((f.fps || 0) > cur.fps) cur.fps = f.fps;
      if ((f.contentLength || 0) > cur.bestSize) cur.bestSize = f.contentLength;
      const c = (f.codecs || "").split(",")[0].split(".")[0].trim();
      if (c && !cur.codec) cur.codec = c;
      heightInfo.set(h, cur);
    }
    if (f.isAudio) {
      hasAudio = true;
      const kbps = Math.round((f.bitrate || 0) / 1000);
      if (kbps > bestAudioKbps) bestAudioKbps = kbps;
    }
  }

  const heights = [...heightInfo.keys()].sort((a, b) => b - a);
  const tag = (h) => h >= 4320 ? " (8K)"
                  : h >= 2160 ? " (4K)"
                  : h >= 1440 ? " (2K)"
                  : h >= 1080 ? " (HD)"
                  : "";
  function fmtBytes(n) {
    if (!n || n <= 0) return "";
    const u = ["B","KB","MB","GB"]; let i = 0, v = n;
    while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
    return (i === 0 ? v.toFixed(0) : v.toFixed(1)) + " " + u[i];
  }

  const opts = [];
  for (const h of heights) {
    const info = heightInfo.get(h);
    const size = fmtBytes(info.bestSize);
    const meta = [
      info.codec || "",
      info.fps && info.fps !== 30 ? `${info.fps}fps` : "",
      size
    ].filter(Boolean).join(" · ");
    opts.push({
      label:    `${h}p${tag(h)}` + (info.fps && info.fps > 30 ? info.fps : ""),
      meta:     meta || "video + audio",
      // Try exact height in mp4+m4a first (clean mux), then any video+audio
      // at exact height, then best ≤H (the page metadata sometimes advertises
      // formats that yt-dlp's chosen player_client doesn't see — fall back
      // gracefully instead of erroring with "Requested format not available").
      ytFormat: `bv*[height=${h}][ext=mp4]+ba[ext=m4a]/bv*[height=${h}]+ba/bv*[height<=${h}]+ba/b[height<=${h}]/best`,
      container:"mp4"
    });
  }
  if (hasAudio || opts.length === 0) {
    opts.push({
      label:    "Audio only",
      meta:     bestAudioKbps ? `${bestAudioKbps} kbps M4A` : "M4A · via yt-dlp",
      ytFormat: "bestaudio[ext=m4a]/bestaudio",
      container:"m4a",
      isAudio:  true
    });
  }
  return opts;
}

let lastUrl = "";
async function maybeExtract(force) {
  if (!force && location.href === lastUrl) return;
  lastUrl = location.href;
  // YouTube populates ytInitialPlayerResponse asynchronously after SPA nav,
  // and Vimeo's player config arrives after iframe boot. Retry a few times.
  for (const delay of [50, 700, 1800, 4000]) {
    await new Promise(r => setTimeout(r, delay));
    if (location.href !== lastUrl) return; // navigated again, abort
    const r = await extractFromBackground();
    if (r && r.ok && r.data && r.data.formats && r.data.formats.length > 0) {
      r.data.pickerOptions = buildYtPickerOptions(r.data);
      console.log("[matata] extracted", r.data.formats.length, "formats →",
                  r.data.pickerOptions.length, "picker options for",
                  r.data.title || r.data.videoId);
      lastMeta = r.data;
      try { chrome.runtime.sendMessage({ action: "page-formats", data: r.data }); } catch {}
      ensureOverlay(lastMeta);
      return;
    }
  }
  // No direct formats — install the yt-dlp fallback overlay if we're on a
  // YouTube watch page.
  const fb = youtubeWatchFallback();
  if (fb) {
    console.log("[matata] using yt-dlp fallback overlay for", fb.videoId || location.href);
    lastMeta = fb;
    try { chrome.runtime.sendMessage({ action: "page-formats", data: fb }); } catch {}
    ensureOverlay(lastMeta);
    return;
  }
  console.log("[matata] no formats extracted from", location.href);
}

// ---- floating "Download this video" overlay -------------------------

let lastMeta    = null;
let overlayHost = null;
let outsideHandler = null;

const overlayCss = `
:host { all: initial; }
.wrap {
  position: relative;
  font-family: "Segoe UI", system-ui, sans-serif;
  user-select: none;
  -webkit-user-select: none;
}
.btn {
  display: inline-flex; align-items: center; gap: 6px;
  background: linear-gradient(135deg, #4ea1ff, #8a5cff 70%, #ff5ca0);
  color: #fff;
  border: none;
  padding: 7px 12px 7px 11px;
  border-radius: 9px;
  cursor: pointer;
  font: 600 12.5px/1 "Segoe UI", system-ui, sans-serif;
  letter-spacing: 0.15px;
  box-shadow: 0 3px 12px rgba(0,0,0,0.45), inset 0 0 0 1px rgba(255,255,255,0.1);
  transition: transform 100ms ease, filter 100ms ease;
}
.btn:hover  { filter: brightness(1.08); }
.btn:active { transform: scale(0.97); }
.btn svg.dl { width: 14px; height: 14px; }
.btn svg.cv { width: 11px; height: 11px; opacity: 0.9; margin-left: 1px; }
.x {
  margin-left: 4px;
  width: 20px; height: 20px;
  display: inline-flex; align-items: center; justify-content: center;
  border-radius: 6px;
  background: rgba(0,0,0,0.4);
  color: #fff;
  border: none;
  cursor: pointer;
  font: 600 11px/1 "Segoe UI", system-ui, sans-serif;
}
.x:hover { background: rgba(0,0,0,0.6); }
.menu {
  display: none;
  position: absolute;
  top: 40px;
  right: 0;
  min-width: 280px;
  max-height: 360px;
  overflow-y: auto;
  background: #1c1f25;
  color: #e6e8ec;
  border: 1px solid #383d46;
  border-radius: 9px;
  padding: 4px;
  box-shadow: 0 8px 28px rgba(0,0,0,0.55);
}
.menu.open { display: block; }
.menu .title {
  font-size: 11px; color: #9aa0aa;
  padding: 6px 10px 4px; text-transform: uppercase; letter-spacing: 0.6px;
}
.item {
  display: flex; justify-content: space-between; align-items: baseline;
  padding: 8px 10px;
  border-radius: 6px;
  cursor: pointer;
  gap: 12px;
  font-size: 12.5px;
}
.item:hover { background: #2c303a; }
.item .q { font-weight: 600; color: #e6e8ec; }
.item .meta { color: #9aa0aa; font-size: 11px; white-space: nowrap; }
.empty { padding: 10px; color: #9aa0aa; font-size: 12px; }
.toast {
  position: absolute;
  top: 40px; right: 0;
  background: #1c1f25; color: #e6e8ec;
  border: 1px solid #383d46; border-left: 3px solid #4ad286;
  border-radius: 8px; padding: 8px 12px; font-size: 12px;
  box-shadow: 0 6px 20px rgba(0,0,0,0.5);
  white-space: nowrap;
}
`;

const overlayHtml = `
<div class="wrap">
  <button class="btn" id="dlBtn">
    <svg class="dl" viewBox="0 0 16 16">
      <path d="M8 2v8m-4-4 4 4 4-4M3 14h10" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>
    </svg>
    <span>Download this video</span>
    <svg class="cv" viewBox="0 0 16 16">
      <path d="M4 6l4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
    </svg>
  </button>
  <button class="x" id="closeBtn" title="Hide for this page">×</button>
  <div class="menu" id="menu"></div>
</div>
`;

function findPlayerAnchor() {
  const h = location.hostname;
  if (h.endsWith("youtube.com")) {
    return document.getElementById("movie_player")
        || document.querySelector(".html5-video-player");
  }
  if (h.endsWith("vimeo.com")) {
    return document.querySelector(".player, [data-player]");
  }
  return null;
}

function fmtSize(n) {
  if (!n || n <= 0) return "";
  const u = ["B","KB","MB","GB"];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
  return (i === 0 ? v.toFixed(0) : v.toFixed(1)) + " " + u[i];
}

function fmtFormat(f) {
  if (f.viaYtDlp) {
    return { q: f.quality || "Download", meta: f.meta || "via yt-dlp" };
  }
  const left = [];
  if (f.height) left.push(f.height + "p" + (f.fps && f.fps !== 30 ? f.fps : ""));
  else if (f.quality) left.push(f.quality);
  if (f.isAudio && !f.isVideo) {
    left.length = 0;
    left.push("Audio");
    if (f.audioBitrate) left.push(Math.round(f.audioBitrate/1000) + " kbps");
  }
  const codecs = (f.codecs || "").split(",")[0].trim().split(".")[0];
  if (codecs) left.push(codecs);
  if (f.container) left.push(f.container.toUpperCase());
  const right = [];
  if (f.contentLength)  right.push(fmtSize(f.contentLength));
  else if (f.bitrate)   right.push(Math.round(f.bitrate/1000) + " kbps");
  return { q: left[0] || "?", meta: left.slice(1).concat(right).join(" · ") };
}

function safeFilename(meta, f) {
  const safe = (meta.title || "video").replace(/[\\/:*?"<>|]+/g, "_").slice(0, 120);
  const ext = f.container || (f.mimeType || "").split("/")[1] || "mp4";
  return `${safe}.${ext}`;
}

function isPageDismissed() {
  try { return sessionStorage.getItem("matata-dismissed") === "1"; }
  catch { return false; }
}

function dismissForPage() {
  try { sessionStorage.setItem("matata-dismissed", "1"); } catch {}
  removeOverlay();
}

function removeOverlay() {
  if (overlayHost) { overlayHost.remove(); overlayHost = null; }
  if (outsideHandler) { document.removeEventListener("click", outsideHandler, true); outsideHandler = null; }
}

function ensureOverlay(meta) {
  if (!meta || !meta.formats || meta.formats.length === 0 || isPageDismissed()) {
    removeOverlay(); return;
  }
  const anchor = findPlayerAnchor();
  if (!anchor) { removeOverlay(); return; }

  // Anchor must be position:relative (or non-static) for our absolute child.
  const cs = getComputedStyle(anchor);
  if (cs.position === "static") anchor.style.position = "relative";

  if (!overlayHost || overlayHost.parentElement !== anchor) {
    removeOverlay();
    overlayHost = document.createElement("div");
    overlayHost.id = "matata-overlay-host";
    overlayHost.style.cssText =
      "position:absolute;top:12px;right:12px;z-index:99999;pointer-events:auto;";
    const shadow = overlayHost.attachShadow({ mode: "closed" });
    const style = document.createElement("style");
    style.textContent = overlayCss;
    shadow.appendChild(style);
    const root = document.createElement("div");
    root.innerHTML = overlayHtml;
    shadow.appendChild(root);
    anchor.appendChild(overlayHost);

    const btn   = shadow.getElementById("dlBtn");
    const close = shadow.getElementById("closeBtn");
    const menu  = shadow.getElementById("menu");

    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const isOpen = menu.classList.toggle("open");
      if (isOpen) renderMenu(shadow, menu);
    });
    close.addEventListener("click", (e) => { e.stopPropagation(); dismissForPage(); });

    outsideHandler = (e) => {
      if (!overlayHost) return;
      if (overlayHost.contains(e.target)) return;
      menu.classList.remove("open");
    };
    document.addEventListener("click", outsideHandler, true);

    overlayHost._shadow = shadow;
  }
  // Re-render menu if open so updated meta is reflected.
  const menu = overlayHost._shadow.getElementById("menu");
  if (menu && menu.classList.contains("open")) renderMenu(overlayHost._shadow, menu);
}

function renderMenu(shadow, menu) {
  if (!lastMeta) { menu.innerHTML = '<div class="empty">No formats detected.</div>'; return; }
  const opts = (lastMeta.pickerOptions && lastMeta.pickerOptions.length)
               ? lastMeta.pickerOptions
               : (Array.isArray(lastMeta.formats) ? lastMeta.formats.map(f => {
                    const lab = fmtFormat(f);
                    return { label: lab.q, meta: lab.meta, raw: f };
                  }) : []);
  let html = '<div class="title">' +
             escapeHtml(lastMeta.title || "Video") +
             '</div>';
  if (opts.length === 0) {
    html += '<div class="empty">No formats available.</div>';
  } else {
    for (let i = 0; i < opts.length; ++i) {
      const o = opts[i];
      html += `<div class="item" data-i="${i}"><span class="q">${escapeHtml(o.label || "?")}</span><span class="meta">${escapeHtml(o.meta || "")}</span></div>`;
    }
  }
  menu.innerHTML = html;
  menu.querySelectorAll(".item").forEach(el => {
    el.addEventListener("click", () => {
      const idx = parseInt(el.dataset.i, 10);
      const o = opts[idx];
      if (!o) return;
      grab(shadow, o);
      menu.classList.remove("open");
    });
  });
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
}

function grab(shadow, opt) {
  // YouTube hands out signed videoplayback URLs that 403 outside the player
  // (SABR + n-cipher). Always send the watch page URL so matata-gui routes
  // it through bundled yt-dlp instead. For other sites (Vimeo etc.) the
  // direct format URL is fine.
  const isYouTubePage = lastMeta && lastMeta.site === "youtube";
  const url = isYouTubePage
              ? (lastMeta.pageUrl || location.href)
              : (opt && opt.raw && opt.raw.url) || (opt && opt.url) || (lastMeta && lastMeta.pageUrl) || location.href;
  // Build a display filename so the confirm dialog and matata GUI row show
  // the real video title — yt-dlp uses its own -o template internally so
  // this name doesn't affect where the file actually lands on disk.
  const filenameFmt = (opt && opt.raw) ? opt.raw : opt;
  let filename = "";
  if (isYouTubePage && lastMeta && lastMeta.title) {
    const safe = lastMeta.title.replace(/[\\/:*?"<>|]+/g, "_").slice(0, 120).trim();
    const ext  = (opt && opt.isAudio) ? "m4a" : ((opt && opt.container) || "mp4");
    filename = safe ? `${safe}.${ext}` : "";
  } else if (!isYouTubePage) {
    filename = safeFilename(lastMeta, filenameFmt || {});
  }
  try {
    chrome.runtime.sendMessage({
      action:   "grab-format",
      url,
      filename,
      referer:  (lastMeta && lastMeta.pageUrl) || location.href,
      ytFormat: (opt && opt.ytFormat) || ""
    }, (resp) => {
      const t = document.createElement("div");
      t.className = "toast";
      t.textContent = (resp && resp.ok) ? "Sent to matata" : "Could not send to matata";
      shadow.querySelector(".wrap").appendChild(t);
      setTimeout(() => t.remove(), 2200);
    });
  } catch {}
}

// Re-anchor the overlay if YouTube swaps DOM around (theater mode, mini
// player, fullscreen). Cheap poll.
setInterval(() => { if (lastMeta) ensureOverlay(lastMeta); }, 1500);

// SPA URL watcher — re-extract on every URL change AND periodically retry
// while we have no formats (the player can populate seconds after onload).
let watchedUrl   = "";
let retryUntil   = 0;
function poke(reason) {
  console.log("[matata] re-extract:", reason);
  try { sessionStorage.removeItem("matata-dismissed"); } catch {}
  // Allow re-running maybeExtract by dropping the lastUrl guard.
  lastUrl = "__force__";
  maybeExtract(true);
}
setInterval(() => {
  if (location.href !== watchedUrl) {
    watchedUrl = location.href;
    retryUntil = Date.now() + 30000; // keep poking for 30s after navigation
    poke("url-change");
    return;
  }
  if (!lastMeta && Date.now() < retryUntil) {
    poke("retry");
  }
}, 1500);

// Initial extract.
retryUntil = Date.now() + 30000;
maybeExtract(true);

})();
