// matata download helper — service worker.
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

function ensurePort() {
  if (nativePort) return nativePort;
  try {
    nativePort = chrome.runtime.connectNative(HOST_NAME);
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
  outDir: ""                     // empty = let host pick Downloads
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
function dispatchToHost(message) {
  const port = ensurePort();
  if (!port) return false;
  try { port.postMessage(message); return true; } catch { return false; }
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

  try {
    const resp = await sendToHost(payload);
    if (!resp || resp.ok !== "true") {
      notify("matata", `host rejected download: ${resp && resp.error || "no reply"}`);
      return true;
    }
    notify("matata", `handed off: ${payload.filename || payload.url}`);
    return true;
  } catch (e) {
    notify("matata", `could not reach matata-host: ${e.message || e}`);
    return true;
  }
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

function mediaTypeOf(url) {
  try {
    const u = new URL(url);
    const p = u.pathname.toLowerCase();
    if (p.endsWith(".m3u8")) return "hls";
    if (p.endsWith(".mpd"))  return "dash";
  } catch {}
  return "";
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

chrome.tabs.onRemoved.addListener((tabId) => { sniffed.delete(tabId); });
chrome.tabs.onUpdated.addListener((tabId, info) => {
  // Clear when the tab navigates to a new page (top-level commit).
  if (info.status === "loading" && info.url) {
    sniffed.delete(tabId);
    try { chrome.action.setBadgeText({ tabId, text: "" }); } catch {}
  }
});

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
  if (msg && msg.action === "list-sniffed") {
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      const tab = tabs && tabs[0];
      if (!tab) { sendResponse({ ok: true, list: [] }); return; }
      sendResponse({ ok: true, list: sniffed.get(tab.id) || [] });
    });
    return true;
  }
  if (msg && msg.action === "grab-sniffed" && msg.url) {
    (async () => {
      const cookie = await cookieHeaderFor(msg.url);
      try {
        const resp = await sendToHost({
          action:    "download",
          url:       msg.url,
          referer:   msg.referer || "",
          cookie,
          userAgent: navigator.userAgent || "",
          filename:  "",
          outDir:    msg.outDir || ""
        });
        sendResponse({ ok: true, r: resp });
      } catch (e) {
        sendResponse({ ok: false, err: String(e.message || e) });
      }
    })();
    return true;
  }
});
