// matata download helper — service worker.
//
// NOTE: extractYouTubeInPage / extractVimeoInPage are SERIALIZED to source
// and injected into the page world via chrome.scripting.executeScript.
// They must reference only globals available in that world (no chrome.*).

function extractYouTubeInPage() {
  try {
    let r = null;
    // 1. Live player API — most reliable for SPA-navigated videos.
    const player = document.getElementById("movie_player") ||
                   document.querySelector(".html5-video-player");
    if (player && typeof player.getPlayerResponse === "function") {
      try { r = player.getPlayerResponse(); } catch (_) {}
    }
    // 2. Initial page-load global.
    if (!r || !r.streamingData) r = window.ytInitialPlayerResponse;
    // 3. Old-style stringified config arg.
    if (!r || !r.streamingData) {
      const c = window.ytplayer && window.ytplayer.config &&
                window.ytplayer.config.args && window.ytplayer.config.args.player_response;
      if (typeof c === "string") { try { r = JSON.parse(c); } catch (_) {} }
      else if (c && typeof c === "object") r = c;
    }
    if (!r || !r.streamingData) return null;
    const sd = r.streamingData;
    const vd = r.videoDetails || {};
    const formats = [].concat(sd.formats || [], sd.adaptiveFormats || [])
      .map(f => {
        const cm = (f.mimeType || "").match(/codecs="([^"]+)"/);
        return {
          itag:        f.itag,
          mimeType:    f.mimeType || "",
          container:   ((f.mimeType || "").split(";")[0] || "").split("/")[1] || "",
          codecs:      cm ? cm[1] : "",
          url:         f.url || "",
          hasCipher:   !!(f.signatureCipher || f.cipher),
          width:       f.width || 0,
          height:      f.height || 0,
          fps:         f.fps || 0,
          bitrate:     f.bitrate || 0,
          contentLength: f.contentLength ? Number(f.contentLength) : 0,
          quality:     f.qualityLabel || f.quality || "",
          audioQuality:f.audioQuality || "",
          audioBitrate:f.averageBitrate || 0,
          isAudio:     /^audio\//.test(f.mimeType || ""),
          isVideo:     /^video\//.test(f.mimeType || "")
        };
      })
      // Keep ALL formats (cipher/SABR included) — we only use them for the
      // quality picker; matata-gui hands the watch URL + selector to yt-dlp.
      .filter(f => f.height || /^audio\//.test(f.mimeType || ""));
    return {
      site:    "youtube",
      videoId: vd.videoId || "",
      title:   vd.title || document.title,
      author:  vd.author || "",
      duration:Number(vd.lengthSeconds || 0),
      pageUrl: location.href,
      formats
    };
  } catch (e) { return null; }
}

function extractVimeoInPage() {
  try {
    const cfg = (window.player && window.player.config) || window.playerConfig || null;
    if (!cfg || !cfg.video) return null;
    const f = (cfg.request && cfg.request.files) || {};
    const formats = [];
    if (f.progressive) for (const v of f.progressive) formats.push({
      mimeType: "video/mp4", container: "mp4", url: v.url,
      width: v.width || 0, height: v.height || 0, fps: v.fps || 0,
      quality: v.quality || (v.height ? v.height + "p" : ""),
      isVideo: true
    });
    if (f.hls && f.hls.cdns) {
      const cdn = f.hls.cdns[f.hls.default_cdn];
      if (cdn && cdn.url) formats.push({
        mimeType: "application/vnd.apple.mpegurl", container: "m3u8",
        url: cdn.url, isHls: true, quality: "auto (HLS)"
      });
    }
    if (formats.length === 0) return null;
    return {
      site: "vimeo",
      videoId: String(cfg.video.id || ""),
      title: cfg.video.title || document.title,
      author: (cfg.video.owner && cfg.video.owner.name) || "",
      duration: Number(cfg.video.duration || 0),
      pageUrl: location.href,
      formats
    };
  } catch (e) { return null; }
}

//
// Strategy: when the browser creates a download whose extension matches the
// user's filter list, we cancel + erase it and hand the URL (plus cookies
// and referer) to the matata native host. The host launches matata.exe
// which does the actual transfer.

const HOST_NAME = "com.matata.host";

// -----------------------------------------------------------------------
// Persistent native-messaging port. Restarted on disconnect so the host
// comes back up after a Chrome relaunch.
let nativePort = null;
// jobId -> {url, status:"queued"|"running"|"done"|"err"|"abort", latest, updatedAt, path, message}
const jobs = new Map();

function resetPort() {
  if (nativePort) {
    try { nativePort.disconnect(); } catch {}
  }
  nativePort = null;
}

function ensurePort() {
  if (nativePort) return nativePort;
  try {
    nativePort = chrome.runtime.connectNative(HOST_NAME);
    console.log("[matata] native port connected to", HOST_NAME);
  } catch (e) {
    console.warn("[matata] connectNative failed:", e);
    return null;
  }
  nativePort.onMessage.addListener((msg) => {
    if (!msg) return;
    const id = msg.jobId;
    if (!id) return;
    const cur = jobs.get(id) || { url: msg.url || "", status: "queued" };
    if (msg.type === "start") { cur.status = "running"; cur.url = msg.url || cur.url; cur.kind = msg.kind; }
    else if (msg.type === "progress") { cur.status = "running"; cur.latest = msg; }
    else if (msg.type === "done")  { cur.status = "done";  cur.path = msg.path; }
    else if (msg.type === "err")   { cur.status = "err";   cur.message = msg.message || ""; }
    else if (msg.type === "abort") { cur.status = "abort"; }
    else if (msg.type === "exit")  { cur.exitCode = msg.code; }
    cur.updatedAt = Date.now();
    jobs.set(id, cur);
  });
  nativePort.onDisconnect.addListener(() => {
    const err = chrome.runtime.lastError;
    if (err) console.warn("[matata] native port disconnected:", err.message);
    nativePort = null;
  });
  return nativePort;
}

const DEFAULT_EXTS = [
  // archives
  "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "iso",
  // video
  "mp4", "mkv", "avi", "mov", "webm", "flv", "mpg", "mpeg", "m4v", "ts",
  // audio
  "mp3", "flac", "wav", "m4a", "aac", "ogg", "opus",
  // programs
  "exe", "msi", "apk", "dmg", "pkg", "deb", "rpm",
  // docs + big binaries
  "pdf", "epub", "mobi",
  // everything else large/binary you might care about
  "bin", "img", "vhd", "vmdk"
];

const DEFAULT_SETTINGS = {
  enabled: true,
  minSize: 1024 * 1024,         // bytes; 0 = intercept anything matching ext
  extensions: DEFAULT_EXTS,
  outDir: "",                    // empty = let host pick Downloads
  confirmEach: true,             // IDM-style: show popup before hijacking
  skipExts: []                   // "don't ask again for .zip" — silently let browser handle
};

async function getSettings() {
  const stored = await chrome.storage.local.get(["settings"]);
  return { ...DEFAULT_SETTINGS, ...(stored.settings || {}) };
}

function extOf(filename) {
  if (!filename) return "";
  const m = filename.match(/\.([A-Za-z0-9]+)$/);
  return m ? m[1].toLowerCase() : "";
}

async function cookieHeaderFor(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies.map(c => `${c.name}=${c.value}`).join("; ");
  } catch {
    return "";
  }
}

// Build a Netscape-format cookies.txt body for yt-dlp's --cookies flag.
// Pulls cookies for the URL's origin AND for the related auth domains
// (YouTube needs google.com SAPISIDHASH cookies; without them yt-dlp gets
// bumped to the "tv downgraded" player_client which has no high-res
// formats). Native messaging frames cap at 1MB, so the full set fits.
async function cookieFileFor(url) {
  let host = "";
  try { host = new URL(url).hostname.toLowerCase(); } catch { return ""; }

  // Domains worth including for each known site. yt-dlp matches by domain
  // suffix so listing the parent (.google.com) covers subdomains.
  const groups = [];
  if (/(?:^|\.)youtube\.com$/.test(host) ||
      /(?:^|\.)youtu\.be$/.test(host) ||
      /(?:^|\.)googlevideo\.com$/.test(host)) {
    groups.push(".youtube.com", ".google.com", ".googlevideo.com",
                ".youtube-nocookie.com", "accounts.google.com");
  } else if (/(?:^|\.)vimeo\.com$/.test(host)) {
    groups.push(".vimeo.com", ".vimeocdn.com");
  } else {
    groups.push(host);
  }

  const seen = new Set();
  const buckets = [];
  for (const d of groups) {
    try {
      const list = await chrome.cookies.getAll({ domain: d.replace(/^\./, "") });
      for (const c of list) {
        const key = (c.domain || "") + "|" + (c.path || "/") + "|" + c.name;
        if (seen.has(key)) continue;
        seen.add(key);
        buckets.push(c);
      }
    } catch {}
  }
  if (buckets.length === 0) return "";

  const lines = ["# Netscape HTTP Cookie File", "# Generated by matata"];
  for (const c of buckets) {
    let dom = c.domain || "";
    // hostOnly cookies in Chrome's cookies API have no leading dot but
    // .hostOnly === true. Netscape format flag is "TRUE" for "include
    // subdomains" which corresponds to a leading-dot domain.
    const includeSub = (!c.hostOnly && dom.startsWith(".")) ? "TRUE" : "FALSE";
    if (!dom.startsWith(".") && !c.hostOnly) dom = "." + dom;
    const path    = c.path || "/";
    const secure  = c.secure ? "TRUE" : "FALSE";
    const expires = c.session ? 0 : Math.floor(c.expirationDate || 0);
    // yt-dlp tolerates tabs in name/value badly; cookie names/values from
    // chrome.cookies are already URL-safe so just strip CR/LF defensively.
    const name  = (c.name  || "").replace(/[\r\n\t]/g, "");
    const value = (c.value || "").replace(/[\r\n\t]/g, "");
    lines.push([dom, includeSub, path, secure, String(expires), name, value].join("\t"));
  }
  return lines.join("\n") + "\n";
}

// One-shot RPC (ping). Uses the transient channel; small and simple.
function sendToHostOnce(message) {
  return new Promise((resolve, reject) => {
    try {
      chrome.runtime.sendNativeMessage(HOST_NAME, message, (response) => {
        const err = chrome.runtime.lastError;
        if (err) reject(err); else resolve(response);
      });
    } catch (e) {
      reject(e);
    }
  });
}

// Fire-and-forget dispatch on the persistent port. The host replies with
// progress/done messages over the same port into our onMessage handler.
// On postMessage failure (stale port), drop it and reconnect once.
function dispatchToHost(message) {
  for (let attempt = 0; attempt < 2; ++attempt) {
    const port = ensurePort();
    if (!port) return false;
    try { port.postMessage(message); return true; }
    catch (e) {
      console.warn("[matata] postMessage failed (attempt " + attempt + "):", e);
      resetPort();
    }
  }
  return false;
}

// Compatibility shim: old call sites used sendToHost() for everything.
// ping still wants a response; downloads now stream, so we just dispatch.
function sendToHost(message) {
  if (message && message.action === "ping") return sendToHostOnce(message);
  const ok = dispatchToHost(message);
  return Promise.resolve(ok ? { ok: "true" } : { ok: "false", error: "no native port" });
}

function suggestedName(filename) {
  if (!filename) return "";
  // Chrome passes an absolute-ish path for filename — keep just the basename.
  const norm = filename.replace(/\\/g, "/");
  const parts = norm.split("/");
  return parts[parts.length - 1] || "";
}

// key -> { payload, settings, fileSize, createdAt }
const pendingConfirms = new Map();
const PENDING_TTL_MS = 5 * 60 * 1000;
let pendingSeq = 0;

function sweepPending() {
  const cutoff = Date.now() - PENDING_TTL_MS;
  for (const [k, v] of pendingConfirms.entries()) {
    if (v.createdAt < cutoff) pendingConfirms.delete(k);
  }
}

// Open the IDM-style confirm dialog with a pre-built payload. If
// confirmEach is off, dispatch immediately. Returns the response shape the
// caller (grab-format / grab-sniffed / ...) can return to its sender.
async function confirmOrHandoff(payload, fileSize) {
  const settings = await getSettings();
  if (!settings.confirmEach) {
    return await handoffToHost(payload);
  }
  sweepPending();
  const key = String(++pendingSeq) + "-" + Date.now().toString(36);
  pendingConfirms.set(key, {
    payload,
    fileSize: fileSize > 0 ? fileSize : 0,
    createdAt: Date.now()
  });
  const url = chrome.runtime.getURL("confirm.html") + "?id=" + encodeURIComponent(key);
  try {
    await chrome.windows.create({
      url, type: "popup", width: 560, height: 340, focused: true
    });
    return { ok: true, queued: "confirm" };
  } catch (e) {
    pendingConfirms.delete(key);
    return await handoffToHost(payload);
  }
}

async function handoffToHost(payload) {
  console.log("[matata] handoff →", payload.url, "filename=" + (payload.filename || ""));
  try {
    const resp = await sendToHost(payload);
    if (!resp || resp.ok !== "true") {
      const err = "host rejected download: " + ((resp && resp.error) || "no reply");
      console.error("[matata]", err);
      notify("matata", err);
      return { ok: false, error: err };
    }
    notify("matata", "handed off: " + (payload.filename || payload.url));
    return { ok: true };
  } catch (e) {
    const err = "could not reach matata-host: " + (e.message || e);
    console.error("[matata]", err);
    notify("matata", err);
    return { ok: false, error: err };
  }
}

async function maybeIntercept(item) {
  const settings = await getSettings();
  if (!settings.enabled) return false;

  // Chrome gives us a negative fileSize until headers are known; skip tiny.
  if (settings.minSize > 0 && item.fileSize > 0 && item.fileSize < settings.minSize) {
    return false;
  }

  const ext = extOf(item.filename) || extOf(item.url.split("?")[0]);
  if (!settings.extensions.includes(ext)) return false;

  // Only http(s). Skip data:, blob:, file:, ftp:.
  if (!/^https?:\/\//i.test(item.url)) return false;

  // "Don't ask again" list — just let the browser handle it silently.
  if (Array.isArray(settings.skipExts) && settings.skipExts.includes(ext)) return false;

  // Cancel the browser's in-progress fetch either way; matata will take over
  // or (on Cancel) the user ends up with nothing, matching IDM's behavior.
  try { await chrome.downloads.cancel(item.id);  } catch {}
  try { await chrome.downloads.erase({ id: item.id }); } catch {}

  const cookie = await cookieHeaderFor(item.finalUrl || item.url);
  const payload = {
    action:    "download",
    url:       item.finalUrl || item.url,
    referer:   item.referrer || "",
    cookie,
    userAgent: navigator.userAgent || "",
    filename:  suggestedName(item.filename),
    outDir:    settings.outDir || ""
  };

  // Warm the port so it's ready when the user clicks Start.
  ensurePort();
  await confirmOrHandoff(payload, item.fileSize > 0 ? item.fileSize : 0);
  return true;
}

function notify(title, message) {
  // Best-effort: Chrome notifications require the "notifications" permission,
  // which we haven't declared to keep the manifest minimal. Log to the
  // service worker console instead.
  console.log(`[${title}] ${message}`);
}

chrome.downloads.onCreated.addListener((item) => {
  maybeIntercept(item).catch(e => console.error("matata intercept error:", e));
});

// -----------------------------------------------------------------------
// Media sniffer. Watches webRequest for URLs that look like HLS/DASH
// manifests and records them per-tab so the popup can offer them up.

const MAX_PER_TAB = 20;
// tabId -> [{url, referer, type, detectedAt}]
const sniffed = new Map();

// Progressive containers we recognise by URL extension. Segment-only formats
// (.ts, .m4s) are deliberately excluded -- HLS/DASH manifests already cover
// them and their per-segment requests would flood the per-tab list.
const PROGRESSIVE_EXTS = {
  ".mp4":  "mp4",
  ".m4v":  "mp4",
  ".webm": "webm",
  ".mkv":  "mkv",
  ".mov":  "mov",
  ".avi":  "avi",
  ".flv":  "flv",
  ".ogv":  "ogv",
  ".3gp":  "3gp",
  ".wmv":  "wmv"
};

function extOf(path) {
  const dot = path.lastIndexOf(".");
  return dot >= 0 ? path.slice(dot) : "";
}

function mediaTypeOf(url) {
  try {
    const u = new URL(url);
    if (u.protocol !== "http:" && u.protocol !== "https:") return "";
    const p = u.pathname.toLowerCase();
    const h = u.hostname.toLowerCase();
    // -- HLS / DASH manifests -----------------------------------------
    if (p.endsWith(".m3u8") || p.includes(".m3u8?")) return "hls";
    if (p.endsWith(".mpd")  || p.includes(".mpd?"))  return "dash";
    // Vimeo's master.json playlist.
    if (h.endsWith("akamaized.net") && /master\.json/.test(p)) return "dash";
    if (h.endsWith("vimeocdn.com")  && /master\.json/.test(p)) return "dash";
    // Bare HLS playlist names some CDNs use.
    if (/\/(playlist|index|chunklist|master)\.m3u8/.test(p))   return "hls";
    // -- Microsoft Smooth Streaming -----------------------------------
    if (/\.ism(?:\/manifest)?\b/.test(p))  return "smooth";
    if (/\/manifest$/.test(p))             return "smooth";
    // -- Progressive media files --------------------------------------
    // Try the basename's extension (handles ?query suffixes).
    const base = p.split(/[?#]/)[0];
    const ext  = extOf(base);
    if (ext && PROGRESSIVE_EXTS[ext]) return PROGRESSIVE_EXTS[ext];
  } catch {}
  return "";
}

// Maps a Content-Type response header to a sniff bucket. Used by the
// onHeadersReceived listener to catch HLS/DASH/progressive responses
// served from URLs that don't carry a recognisable extension.
const CONTENT_TYPE_MAP = {
  "application/vnd.apple.mpegurl":  "hls",
  "application/x-mpegurl":          "hls",
  "audio/mpegurl":                  "hls",   // some servers misname it
  "audio/x-mpegurl":                "hls",
  "application/dash+xml":           "dash",
  "video/vnd.mpeg.dash.mpd":        "dash",
  "video/mp4":                      "mp4",
  "video/x-m4v":                    "mp4",
  "video/webm":                     "webm",
  "video/x-matroska":               "mkv",
  "video/quicktime":                "mov",
  "video/x-msvideo":                "avi",
  "video/x-flv":                    "flv",
  "video/ogg":                      "ogv",
  "video/3gpp":                     "3gp",
  "video/x-ms-wmv":                 "wmv"
};

// Tiny progressive responses are almost always thumbnails / previews /
// adverts -- skip anything below this length to keep the badge clean.
const MIN_PROGRESSIVE_BYTES = 262144; // 256 KB

function bucketIsProgressive(t) {
  return t && t !== "hls" && t !== "dash" && t !== "smooth";
}

function findHeader(headers, name) {
  if (!headers) return "";
  const lname = name.toLowerCase();
  for (const h of headers) {
    if (h.name && h.name.toLowerCase() === lname) return String(h.value || "");
  }
  return "";
}

function typeFromContentType(ct) {
  if (!ct) return "";
  const main = ct.split(";")[0].trim().toLowerCase();
  return CONTENT_TYPE_MAP[main] || "";
}

function pushSniffed(tabId, entry) {
  if (tabId === undefined || tabId < 0) return;
  let list = sniffed.get(tabId) || [];
  if (list.some(e => e.url === entry.url)) return;
  list.unshift(entry);
  if (list.length > MAX_PER_TAB) list = list.slice(0, MAX_PER_TAB);
  sniffed.set(tabId, list);

  // Badge the toolbar icon with the count (best-effort).
  try {
    chrome.action.setBadgeText({ tabId, text: String(list.length) });
    chrome.action.setBadgeBackgroundColor({ tabId, color: "#1a7f1a" });
  } catch {}
}

chrome.webRequest.onBeforeRequest.addListener(
  (details) => {
    const t = mediaTypeOf(details.url);
    if (!t) return;
    pushSniffed(details.tabId, {
      url: details.url,
      type: t,
      referer: details.initiator || details.documentUrl || "",
      detectedAt: Date.now()
    });
  },
  { urls: ["<all_urls>"] }
);

// Header-based fallback. Catches HLS/DASH/progressive responses whose URL
// gives no clue (e.g. /api/stream?id=…). Progressive responses are gated by
// Content-Length so we don't badge thumbnails or 1-second ad clips.
chrome.webRequest.onHeadersReceived.addListener(
  (details) => {
    if (!details || !details.responseHeaders) return;
    // Skip if the URL-pass already classified this exact request.
    if (mediaTypeOf(details.url)) return;
    const ct = findHeader(details.responseHeaders, "content-type");
    const t  = typeFromContentType(ct);
    if (!t) return;
    if (bucketIsProgressive(t)) {
      const lenStr = findHeader(details.responseHeaders, "content-length");
      const len    = lenStr ? parseInt(lenStr, 10) : 0;
      // No length header at all -- chunked transfer; allow it through.
      // Has a length but is tiny -- skip.
      if (len > 0 && len < MIN_PROGRESSIVE_BYTES) return;
    }
    pushSniffed(details.tabId, {
      url: details.url,
      type: t,
      referer: details.initiator || details.documentUrl || "",
      detectedAt: Date.now()
    });
  },
  { urls: ["<all_urls>"] },
  ["responseHeaders"]
);

chrome.tabs.onRemoved.addListener((tabId) => {
  sniffed.delete(tabId);
  pageFormats.delete(tabId);
});
chrome.tabs.onUpdated.addListener((tabId, info) => {
  // Clear when the tab navigates to a new page (top-level commit).
  if (info.status === "loading" && info.url) {
    sniffed.delete(tabId);
    pageFormats.delete(tabId);
    try { chrome.action.setBadgeText({ tabId, text: "" }); } catch {}
  }
});

// -----------------------------------------------------------------------
// Page-extracted formats (YouTube / Vimeo via content script).
// tabId -> { site, videoId, title, author, duration, pageUrl, formats: [...] }
const pageFormats = new Map();

// Allow the popup to ping the host (and verify install correctness).
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.action === "ping-host") {
    sendToHost({ action: "ping" })
      .then(r => sendResponse({ ok: true, r }))
      .catch(e => sendResponse({ ok: false, err: String(e.message || e) }));
    return true; // async response
  }
  if (msg && msg.action === "manual-download" && msg.url) {
    sendToHost({
      action:  "download",
      url:     msg.url,
      referer: msg.referer || "",
      cookie:  "",
      userAgent: navigator.userAgent || "",
      filename: msg.filename || "",
      outDir:   msg.outDir || ""
    })
      .then(r => sendResponse({ ok: true, r }))
      .catch(e => sendResponse({ ok: false, err: String(e.message || e) }));
    return true;
  }
  if (msg && msg.action === "list-jobs") {
    const snapshot = [];
    for (const [id, j] of jobs.entries()) snapshot.push({ id, ...j });
    // Newest first, cap to 20 for display.
    snapshot.sort((a, b) => (b.updatedAt || 0) - (a.updatedAt || 0));
    sendResponse({ ok: true, jobs: snapshot.slice(0, 20) });
    return true;
  }
  if (msg && msg.action === "inspect-stream" && msg.url) {
    // Fetch + parse an HLS master playlist so the overlay can label each
    // variant with resolution/bitrate. Done in the background (not the
    // content script) so we don't run into per-page CORS restrictions —
    // host_permissions: <all_urls> lets us fetch the manifest directly.
    (async () => {
      try {
        const r = await fetch(msg.url, { credentials: "include" });
        const txt = await r.text();
        // Quick HLS shape detection.
        if (!/^#EXTM3U/m.test(txt)) {
          sendResponse({ ok: true, kind: "unknown", variants: [] });
          return;
        }
        const variants = [];
        const lines = txt.split(/\r?\n/);
        for (let i = 0; i < lines.length; ++i) {
          const l = lines[i];
          if (!l.startsWith("#EXT-X-STREAM-INF:")) continue;
          const attrs = l.slice("#EXT-X-STREAM-INF:".length);
          const get = (re) => { const m = attrs.match(re); return m ? m[1] : ""; };
          const reso = get(/RESOLUTION=(\d+x\d+)/i);
          const bw   = parseInt(get(/(?:AVERAGE-)?BANDWIDTH=(\d+)/i) || "0", 10);
          const codecs = get(/CODECS="([^"]+)"/i);
          let next = "";
          for (let j = i + 1; j < lines.length; ++j) {
            const t = lines[j].trim();
            if (!t || t.startsWith("#")) continue;
            next = t; break;
          }
          if (!next) continue;
          let absUrl = next;
          try { absUrl = new URL(next, msg.url).href; } catch {}
          variants.push({
            url: absUrl,
            resolution: reso,
            height: reso ? parseInt(reso.split("x")[1], 10) : 0,
            bandwidth: bw,
            codecs
          });
        }
        // Master if at least one variant was found, otherwise it's a media
        // (chunk) playlist — caller should treat it as a single stream.
        const kind = variants.length > 0 ? "master" : "media";
        sendResponse({ ok: true, kind, variants });
      } catch (e) {
        sendResponse({ ok: false, error: String(e && e.message || e) });
      }
    })();
    return true; // async response
  }
  if (msg && msg.action === "report-dom-media" && Array.isArray(msg.urls)) {
    // Content-script reports URLs scraped from <video src=…> / <source src=…>
    // / video.currentSrc. These often resolve before the network request fires
    // (or the user never hits play), so we synthesise sniffed entries from the
    // DOM and let the overlay surface them anyway.
    const senderTabId = sender && sender.tab && sender.tab.id;
    const ref = (sender && sender.tab && sender.tab.url) || "";
    if (senderTabId !== undefined && senderTabId >= 0) {
      for (const raw of msg.urls) {
        if (!raw || typeof raw !== "string") continue;
        if (raw.startsWith("blob:") || raw.startsWith("data:")) continue;
        const t = mediaTypeOf(raw);
        if (!t) continue;
        pushSniffed(senderTabId, {
          url: raw,
          type: t,
          referer: ref,
          detectedAt: Date.now(),
          source: "dom"
        });
      }
    }
    sendResponse({ ok: true });
    return true;
  }
  if (msg && msg.action === "list-sniffed") {
    // Prefer the sender tab when this comes from a content script, fall back
    // to the active tab when invoked from the popup (no sender.tab there).
    const senderTabId = sender && sender.tab && sender.tab.id;
    if (senderTabId !== undefined && senderTabId >= 0) {
      sendResponse({ ok: true, list: sniffed.get(senderTabId) || [] });
      return true;
    }
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      const tab = tabs && tabs[0];
      if (!tab) { sendResponse({ ok: true, list: [] }); return; }
      sendResponse({ ok: true, list: sniffed.get(tab.id) || [] });
    });
    return true;
  }
  if (msg && msg.action === "get-pending" && msg.key) {
    const entry = pendingConfirms.get(msg.key);
    if (!entry) { sendResponse({ ok: false, error: "expired" }); return true; }
    sendResponse({
      ok: true,
      item: {
        url:      entry.payload.url,
        referer:  entry.payload.referer,
        filename: entry.payload.filename,
        outDir:   entry.payload.outDir,
        fileSize: entry.fileSize
      }
    });
    return true;
  }
  if (msg && msg.action === "confirm-pending" && msg.key) {
    const entry = pendingConfirms.get(msg.key);
    if (!entry) { sendResponse({ ok: false, error: "expired" }); return true; }
    pendingConfirms.delete(msg.key);
    console.log("[matata] confirm-pending choice=" + msg.choice + " key=" + msg.key);

    (async () => {
      // "Don't ask again for .ext" — persist to settings regardless of choice.
      if (msg.dontAskExt) {
        const stored = await chrome.storage.local.get(["settings"]);
        const s = { ...DEFAULT_SETTINGS, ...(stored.settings || {}) };
        const list = new Set(s.skipExts || []);
        list.add(String(msg.dontAskExt).toLowerCase());
        s.skipExts = [...list];
        await chrome.storage.local.set({ settings: s });
      }
      let result = { ok: true };
      if (msg.choice === "start") {
        result = await handoffToHost(entry.payload);
      } else if (msg.choice === "later") {
        result = await handoffToHost({ ...entry.payload, queued: true });
      }
      // "cancel" — do nothing; browser copy is already erased.
      sendResponse(result);
    })();
    return true;
  }
  if (msg && msg.action === "extract-page-formats") {
    const tabId = sender && sender.tab && sender.tab.id;
    if (tabId == null) { sendResponse({ ok: false, error: "no tab" }); return true; }
    const site = msg.site || "";
    const fn = site === "vimeo" ? extractVimeoInPage : extractYouTubeInPage;
    chrome.scripting.executeScript({
      target: { tabId, frameIds: [0] },
      world:  "MAIN",
      func:   fn
    }).then(results => {
      const data = results && results[0] && results[0].result;
      sendResponse({ ok: !!data, data: data || null });
    }).catch(e => sendResponse({ ok: false, error: String(e && e.message || e) }));
    return true;
  }
  if (msg && msg.action === "page-formats" && msg.data) {
    const tabId = sender && sender.tab && sender.tab.id;
    if (tabId !== undefined && tabId >= 0) {
      const data = msg.data;
      // Sort formats: video-with-audio (combined) > video-only > audio-only.
      // Within each, descending by height/bitrate.
      const fs = (data.formats || []).slice();
      fs.sort((a, b) => {
        const aRank = (a.isVideo && !a.isAudio && (a.audioBitrate||0) === 0) ? 1 : 0;
        const bRank = (b.isVideo && !b.isAudio && (b.audioBitrate||0) === 0) ? 1 : 0;
        if (aRank !== bRank) return aRank - bRank;
        const ah = a.height || 0, bh = b.height || 0;
        if (ah !== bh) return bh - ah;
        return (b.bitrate || 0) - (a.bitrate || 0);
      });
      pageFormats.set(tabId, { ...data, formats: fs });
      try {
        const txt = String(fs.filter(f => f.isVideo || f.isHls || f.isDash).length || fs.length);
        chrome.action.setBadgeText({ tabId, text: txt });
        chrome.action.setBadgeBackgroundColor({ tabId, color: "#c4302b" });
      } catch {}
    }
    sendResponse({ ok: true });
    return true;
  }
  if (msg && msg.action === "list-page-formats") {
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      const tab = tabs && tabs[0];
      if (!tab) { sendResponse({ ok: true, data: null }); return; }
      sendResponse({ ok: true, data: pageFormats.get(tab.id) || null });
    });
    return true;
  }
  if (msg && msg.action === "grab-format" && msg.url) {
    (async () => {
      // Skip the Cookie HEADER for YouTube watch URLs — when sent on the
      // command line Google's cookie set easily blows past Windows' 32KB
      // CreateProcess limit. Instead we ship a Netscape cookies.txt
      // body in `cookieFile`, which the native host writes to %TEMP% and
      // hands to yt-dlp as `--cookies <path>`. That way yt-dlp sees the
      // page-fresh SAPISIDHASH / VISITOR_INFO_LIVE and avoids the
      // "tv downgraded" player_client (which has no high-res formats).
      const isYtPage = /^https?:\/\/(?:www\.|m\.|music\.)?youtube\.com\/(?:watch|shorts|playlist)/i.test(msg.url)
                    || /^https?:\/\/youtu\.be\//i.test(msg.url);
      const cookie     = isYtPage ? "" : await cookieHeaderFor(msg.url);
      const cookieFile = isYtPage ? await cookieFileFor(msg.url) : "";
      const payload = {
        action:    "download",
        url:       msg.url,
        referer:   msg.referer || "",
        cookie,
        cookieFile,
        userAgent: navigator.userAgent || "",
        filename:  msg.filename || "",
        outDir:    msg.outDir || "",
        ytFormat:  msg.ytFormat || ""
      };
      const r = await confirmOrHandoff(payload, 0);
      sendResponse(r);
    })();
    return true;
  }
  if (msg && msg.action === "grab-sniffed" && msg.url) {
    (async () => {
      const cookie = await cookieHeaderFor(msg.url);
      const payload = {
        action:    "download",
        url:       msg.url,
        referer:   msg.referer || "",
        cookie,
        userAgent: navigator.userAgent || "",
        filename:  msg.filename || "",
        outDir:    msg.outDir || ""
      };
      const r = await confirmOrHandoff(payload, 0);
      sendResponse(r);
    })();
    return true;
  }
});
