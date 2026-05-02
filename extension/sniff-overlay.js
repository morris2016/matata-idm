// Generic in-page overlay for non-YouTube/Vimeo sites.
//
// The webRequest sniffer in background.js catches every .m3u8/.mpd request
// the tab makes (including from third-party iframes like Vidsrc / Wootly).
// This content script renders a floating "Download this video" button on
// the host page once at least one stream has been seen, mirroring the UX
// of yt-extract.js.

(() => {
"use strict";

// Skip on sites that already have a dedicated overlay.
const host = location.hostname;
if (/(?:^|\.)(?:youtube\.com|youtu\.be|vimeo\.com)$/.test(host)) return;

// Don't render the overlay inside iframes — it would duplicate.
if (window.top !== window) return;

console.log("[matata] sniff-overlay loaded on", location.href);

let overlayHost = null;
let outsideHandler = null;
let lastList = [];
let dismissed = false;

const overlayCss = `
:host { all: initial; }
.wrap {
  position: fixed;
  top: 16px; right: 16px;
  z-index: 2147483646;
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
  top: 40px; right: 0;
  min-width: 320px; max-width: 460px;
  max-height: 360px; overflow-y: auto;
  background: #1c1f25; color: #e6e8ec;
  border: 1px solid #383d46; border-radius: 9px;
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
  padding: 8px 10px; gap: 12px;
  border-radius: 6px; cursor: pointer; font-size: 12.5px;
}
.item:hover { background: #2c303a; }
.item .q { font-weight: 600; color: #e6e8ec; text-transform: uppercase; }
.item .meta {
  color: #9aa0aa; font-size: 11px;
  font-family: Consolas, monospace;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  flex: 1; min-width: 0;
}
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
    <span id="btnLabel">Download this video</span>
    <svg class="cv" viewBox="0 0 16 16">
      <path d="M4 6l4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
    </svg>
  </button>
  <button class="x" id="closeBtn" title="Hide on this page">×</button>
  <div class="menu" id="menu"></div>
</div>
`;

function shorten(url) {
  try {
    const u = new URL(url);
    let p = u.pathname;
    if (p.length > 60) p = p.slice(0, 30) + "…" + p.slice(-25);
    return u.host + p;
  } catch { return url; }
}

function removeOverlay() {
  if (overlayHost) { overlayHost.remove(); overlayHost = null; }
  if (outsideHandler) {
    document.removeEventListener("click", outsideHandler, true);
    outsideHandler = null;
  }
}

function ensureOverlay(list) {
  if (dismissed || !list || list.length === 0) { removeOverlay(); return; }
  if (!document.body) return;

  if (!overlayHost) {
    overlayHost = document.createElement("div");
    overlayHost.id = "matata-sniff-overlay";
    const shadow = overlayHost.attachShadow({ mode: "closed" });
    const style = document.createElement("style");
    style.textContent = overlayCss;
    shadow.appendChild(style);
    const root = document.createElement("div");
    root.innerHTML = overlayHtml;
    shadow.appendChild(root);
    document.body.appendChild(overlayHost);

    const btn   = shadow.getElementById("dlBtn");
    const close = shadow.getElementById("closeBtn");
    const menu  = shadow.getElementById("menu");

    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const isOpen = menu.classList.toggle("open");
      if (isOpen) renderMenu(shadow, menu);
    });
    close.addEventListener("click", (e) => {
      e.stopPropagation();
      dismissed = true;
      removeOverlay();
    });

    outsideHandler = (e) => {
      if (!overlayHost) return;
      if (overlayHost.contains(e.target)) return;
      menu.classList.remove("open");
    };
    document.addEventListener("click", outsideHandler, true);

    overlayHost._shadow = shadow;
  }

  const shadow = overlayHost._shadow;
  const label = shadow.getElementById("btnLabel");
  if (label) label.textContent =
    list.length === 1 ? "Download this video"
                      : `Download (${list.length} streams)`;
  const menu = shadow.getElementById("menu");
  if (menu && menu.classList.contains("open")) renderMenu(shadow, menu);
}

function fmtBandwidth(bps) {
  if (!bps || bps <= 0) return "";
  if (bps >= 1_000_000) return (bps / 1_000_000).toFixed(1) + " Mbps";
  return Math.round(bps / 1000) + " kbps";
}

function qualityLabel(v) {
  if (v.height) {
    let tag = "";
    if (v.height >= 4320) tag = " (8K)";
    else if (v.height >= 2160) tag = " (4K)";
    else if (v.height >= 1440) tag = " (2K)";
    else if (v.height >= 1080) tag = " (HD)";
    return v.height + "p" + tag;
  }
  if (v.resolution) return v.resolution;
  return v.kind === "media" ? "HLS playlist" : "stream";
}

let menuRenderToken = 0;

async function renderMenu(shadow, menu) {
  const token = ++menuRenderToken;
  const baseStreams = lastList.slice();

  // Initial render with the URLs we already know about — gets replaced once
  // each master manifest finishes inspection.
  let html = '<div class="title">Streams detected on this page</div>';
  if (baseStreams.length === 0) {
    html += '<div class="empty" style="padding:10px;color:#9aa0aa;font-size:12px">' +
            'No streams sniffed yet. Click play first.</div>';
  } else {
    html += '<div class="empty" style="padding:6px 10px;color:#9aa0aa;font-size:11px">' +
            'Inspecting playlists…</div>';
  }
  menu.innerHTML = html;

  // Inspect each sniffed URL in parallel; turn master playlists into a flat
  // list of variants tagged with which sniffed entry they came from.
  const inspections = await Promise.all(baseStreams.map(s =>
    new Promise((resolve) => {
      try {
        chrome.runtime.sendMessage({ action: "inspect-stream", url: s.url }, (resp) => {
          resolve({ s, resp: resp || { ok: false } });
        });
      } catch { resolve({ s, resp: { ok: false } }); }
    })
  ));
  if (token !== menuRenderToken) return; // menu was re-rendered or closed

  const flat = [];
  for (const { s, resp } of inspections) {
    if (resp.ok && resp.kind === "master" && resp.variants && resp.variants.length) {
      // Sort variants high-to-low by height/bandwidth.
      const vs = resp.variants.slice().sort((a, b) =>
        (b.height || 0) - (a.height || 0) || (b.bandwidth || 0) - (a.bandwidth || 0));
      for (const v of vs) {
        flat.push({
          url:     v.url,
          referer: s.referer || location.href,
          label:   qualityLabel(v),
          meta:    [v.codecs || "", fmtBandwidth(v.bandwidth)].filter(Boolean).join(" · "),
          parent:  s.url,
          kind:    "variant"
        });
      }
    } else {
      // Either inspection failed or this is already a media playlist (no
      // sub-variants). Surface it as a single entry.
      flat.push({
        url:     s.url,
        referer: s.referer || location.href,
        label:   resp.ok && resp.kind === "media" ? "HLS playlist" : (s.type || "stream").toUpperCase(),
        meta:    shorten(s.url),
        kind:    resp.ok ? resp.kind : "unknown"
      });
    }
  }

  // De-dup by URL (some sites sniff master twice on rebuffer).
  const seenUrl = new Set();
  const items = flat.filter(f => {
    if (seenUrl.has(f.url)) return false;
    seenUrl.add(f.url);
    return true;
  });

  let html2 = '<div class="title">Streams detected on this page</div>';
  if (items.length === 0) {
    html2 += '<div class="empty" style="padding:10px;color:#9aa0aa;font-size:12px">' +
             'No streams found.</div>';
  } else {
    for (let i = 0; i < items.length; ++i) {
      const it = items[i];
      html2 += `<div class="item" data-i="${i}">
        <span class="q">${escapeHtml(it.label)}</span>
        <span class="meta" title="${escapeAttr(it.url)}">${escapeHtml(it.meta || "")}</span>
      </div>`;
    }
  }
  menu.innerHTML = html2;
  menu.querySelectorAll(".item").forEach(el => {
    el.addEventListener("click", () => {
      const idx = parseInt(el.dataset.i, 10);
      const it = items[idx];
      if (!it) return;
      grab(shadow, { url: it.url, referer: it.referer });
      menu.classList.remove("open");
    });
  });
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
}
function escapeAttr(s) { return escapeHtml(s); }

function pageTitleAsFilename() {
  // Strip junk like " | Watch Online" / " — Title - HD" suffixes various
  // streaming sites add, then sanitize for filesystem.
  let t = (document.title || "").trim();
  t = t.replace(/\s*[\|\-–—]\s*(?:watch|stream|free|hd|full\s*movie|online|goojara.*|vidsrc.*|wootly.*).*$/i, "");
  t = t.replace(/[\\/:*?"<>|]+/g, "_").trim();
  if (t.length > 120) t = t.slice(0, 120).trim();
  return t ? t + ".mp4" : "";
}

function grab(shadow, s) {
  try {
    chrome.runtime.sendMessage({
      action:   "grab-sniffed",
      url:      s.url,
      referer:  s.referer || location.href,
      filename: pageTitleAsFilename()
    }, (resp) => {
      const t = document.createElement("div");
      t.className = "toast";
      t.textContent = (resp && resp.ok) ? "Sent to matata" : "Could not send to matata";
      shadow.querySelector(".wrap").appendChild(t);
      setTimeout(() => t.remove(), 2200);
    });
  } catch {}
}

function poll() {
  try {
    chrome.runtime.sendMessage({ action: "list-sniffed" }, (resp) => {
      if (!resp || !resp.ok) return;
      const list = (resp.list || []).filter(e => e && e.url);
      // Only update DOM if changed.
      const sig = list.map(e => e.url).join("|");
      const prevSig = lastList.map(e => e.url).join("|");
      if (sig !== prevSig) {
        lastList = list;
        ensureOverlay(lastList);
      }
    });
  } catch {}
}

// Pull URLs out of any <video> / <source> elements on the page so we catch
// videos the user never clicks "play" on (lazy-loaded players, autoplay-off
// muted previews, etc.) and videos served without a recognisable URL extension
// where the network request might use a generic /api/stream path.
function scanDomMedia() {
  const urls = new Set();
  try {
    const vids = document.querySelectorAll("video, audio");
    for (const v of vids) {
      if (v.currentSrc) urls.add(v.currentSrc);
      if (v.src)        urls.add(v.src);
      const sources = v.querySelectorAll("source");
      for (const s of sources) {
        if (s.src) urls.add(s.src);
      }
    }
  } catch {}
  if (!urls.size) return;
  try {
    chrome.runtime.sendMessage({
      action: "report-dom-media",
      urls:   Array.from(urls)
    });
  } catch {}
}

// Reset dismissed state on URL change so SPA navigation re-shows the overlay.
let watchedUrl = location.href;
setInterval(() => {
  if (location.href !== watchedUrl) {
    watchedUrl = location.href;
    dismissed = false;
    lastList = [];
    removeOverlay();
  }
  scanDomMedia();
  poll();
}, 1500);

scanDomMedia();
poll();

})();
