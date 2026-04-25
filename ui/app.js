// matata UI controller. Talks to the C++ host via window.chrome.webview.
//
// Outbound (UI → host): chrome.webview.postMessage({type, ...})
// Inbound  (host → UI):  window.matata.onEvent({type, ...})

(() => {
"use strict";

const $ = (id) => document.getElementById(id);

const state = {
  items:    new Map(),   // id -> item
  filter:   "all",
  search:   "",
  settings: {}
};

// ---- bridge ---------------------------------------------------------

const isHosted = !!(window.chrome && window.chrome.webview);
function send(msg) {
  if (isHosted) window.chrome.webview.postMessage(JSON.stringify(msg));
  else console.log("[matata] (no host) →", msg);
}

window.matata = {
  onEvent(ev) {
    try {
      switch (ev.type) {
        case "items":      onItems(ev.list); break;
        case "item":       onItem(ev.item); break;
        case "remove":     onRemove(ev.id); break;
        case "settings":   onSettings(ev.settings); break;
        case "toast":      toast(ev.message, ev.kind || "info"); break;
        case "openAdd":    openModal("addModal"); break;
        case "windowState": onWindowState(ev); break;
      }
    } catch (e) { console.error("[matata] onEvent error", e); }
  }
};

// ---- list rendering -------------------------------------------------

function fmtBytes(n) {
  if (n === undefined || n === null || n < 0) return "—";
  const u = ["B","KB","MB","GB","TB"];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
  return (i === 0 ? v.toFixed(0) : v.toFixed(2)) + " " + u[i];
}

function fmtRate(bps) {
  if (!bps || bps <= 0) return "";
  return fmtBytes(bps) + "/s";
}

function fmtETA(secs) {
  if (!secs || secs < 0 || !isFinite(secs)) return "";
  if (secs < 60)   return Math.round(secs) + "s";
  if (secs < 3600) return Math.floor(secs/60) + "m " + Math.round(secs%60) + "s";
  const h = Math.floor(secs/3600), m = Math.floor((secs%3600)/60);
  return h + "h " + m + "m";
}

function kindOf(filename, url) {
  const name = (filename || url || "").toLowerCase();
  const m = name.match(/\.([a-z0-9]+)(?:[?#].*)?$/);
  if (!m) return "file";
  const ext = m[1];
  if (["zip","rar","7z","tar","gz","bz2","xz","iso"].includes(ext)) return "archive";
  if (["mp4","mkv","avi","mov","webm","flv","m4v","ts","mpg","mpeg","m3u8","mpd"].includes(ext)) return "video";
  if (["mp3","flac","wav","m4a","aac","ogg","opus"].includes(ext)) return "audio";
  if (["exe","msi","apk","dmg","pkg","deb","rpm"].includes(ext)) return "program";
  if (["pdf","epub","mobi","doc","docx","txt","xls","xlsx","ppt","pptx"].includes(ext)) return "document";
  return "file";
}

function extLabel(filename, url) {
  const name = filename || url || "";
  const m = name.match(/\.([a-z0-9]+)(?:[?#].*)?$/i);
  return m ? m[1].slice(0,4).toUpperCase() : "?";
}

function statusOf(item) {
  // server-side states: queued, running, done, err, aborted
  return item.state || "queued";
}

function nameOf(item) {
  if (item.filename) return item.filename;
  try {
    const u = new URL(item.url);
    const p = u.pathname.split("/").filter(Boolean);
    return decodeURIComponent(p[p.length - 1] || u.host);
  } catch { return item.url || "(unknown)"; }
}

function rowMatchesFilter(item) {
  const f = state.filter;
  const s = state.search;
  if (s) {
    const hay = (nameOf(item) + " " + (item.url || "")).toLowerCase();
    if (!hay.includes(s)) return false;
  }
  if (f === "all") return true;
  const st = statusOf(item);
  if (f === "running" || f === "queued" || f === "done" || f === "err") {
    return st === f;
  }
  // category filter
  return kindOf(item.filename, item.url) === categoryToKind(f);
}

function categoryToKind(cat) {
  switch (cat) {
    case "compressed": return "archive";
    case "documents":  return "document";
    case "music":      return "audio";
    case "programs":   return "program";
    case "video":      return "video";
  }
  return "";
}

function renderRow(item) {
  const status = statusOf(item);
  const kind   = kindOf(item.filename, item.url);
  const name   = nameOf(item);
  const pct    = item.total > 0
    ? Math.max(0, Math.min(100, item.downloaded * 100 / item.total))
    : (status === "done" ? 100 : 0);

  let row = document.querySelector(`.row[data-id="${item.id}"]`);
  if (!row) {
    row = document.createElement("div");
    row.className = "row";
    row.dataset.id = String(item.id);
    row.innerHTML = `
      <div class="row-icon" data-kind="${kind}"></div>
      <div class="row-main">
        <div class="row-line1">
          <div class="row-name"></div>
          <div class="row-pill"></div>
        </div>
        <div class="row-progress"><div class="row-progress-fill"></div></div>
        <div class="row-meta"></div>
      </div>
      <div class="row-actions"></div>
    `;
    $("rows").appendChild(row);
  }

  row.classList.toggle("done",    status === "done");
  row.classList.toggle("err",     status === "err");
  row.classList.toggle("aborted", status === "aborted");

  row.querySelector(".row-icon").dataset.kind = kind;
  row.querySelector(".row-icon").textContent = extLabel(item.filename, item.url);
  row.querySelector(".row-name").textContent = name;
  row.querySelector(".row-name").title = item.url || "";
  const pill = row.querySelector(".row-pill");
  pill.className = "row-pill " + status;
  pill.textContent = status;
  row.querySelector(".row-progress-fill").style.width = pct.toFixed(2) + "%";

  const meta = row.querySelector(".row-meta");
  const parts = [];
  if (item.total > 0) {
    parts.push(`<span><b>${fmtBytes(item.downloaded||0)}</b> / ${fmtBytes(item.total)}</span>`);
    parts.push(`<span>${pct.toFixed(1)}%</span>`);
  } else if (item.downloaded > 0) {
    parts.push(`<span><b>${fmtBytes(item.downloaded)}</b></span>`);
  }
  if (status === "running") {
    if (item.bps > 0)         parts.push(`<span>${fmtRate(item.bps)}</span>`);
    if (item.activeConns > 0) parts.push(`<span>${item.activeConns} conn</span>`);
    if (item.total > 0 && item.bps > 0) {
      const eta = (item.total - item.downloaded) / item.bps;
      parts.push(`<span>${fmtETA(eta)} left</span>`);
    }
  } else if (status === "err" && item.message) {
    parts.push(`<span style="color:var(--err)">${escapeHtml(item.message)}</span>`);
  } else if (status === "done" && item.resultPath) {
    parts.push(`<span title="${escapeHtml(item.resultPath)}">saved</span>`);
  }
  meta.innerHTML = parts.join("");

  // Per-state action buttons
  const actions = row.querySelector(".row-actions");
  actions.innerHTML = actionButtons(status);
}

function actionButtons(status) {
  const ic = (title, cls, svg) =>
    `<button class="row-action" data-act="${cls}" title="${title}">${svg}</button>`;
  const playSvg   = `<svg viewBox="0 0 16 16"><path d="M5 3l8 5-8 5z" fill="currentColor"/></svg>`;
  const pauseSvg  = `<svg viewBox="0 0 16 16"><rect x="4" y="3" width="3" height="10" rx="1" fill="currentColor"/><rect x="9" y="3" width="3" height="10" rx="1" fill="currentColor"/></svg>`;
  const folderSvg = `<svg viewBox="0 0 16 16"><path d="M2 4a1 1 0 0 1 1-1h3l1.5 1.5H13a1 1 0 0 1 1 1V12a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V4z" fill="none" stroke="currentColor" stroke-width="1.2"/></svg>`;
  const fileSvg   = `<svg viewBox="0 0 16 16"><path d="M4 2h5l3 3v9H4V2z" fill="none" stroke="currentColor" stroke-width="1.2"/></svg>`;
  const trashSvg  = `<svg viewBox="0 0 16 16"><path d="M3 5h10M6 5V3h4v2M5 5l1 9h4l1-9" fill="none" stroke="currentColor" stroke-width="1.2"/></svg>`;
  const retrySvg  = `<svg viewBox="0 0 16 16"><path d="M13 8a5 5 0 1 1-1.5-3.5L13 3v4h-4" fill="none" stroke="currentColor" stroke-width="1.2"/></svg>`;

  const out = [];
  if (status === "running") out.push(ic("Pause", "pause", pauseSvg));
  if (status === "queued" || status === "aborted") out.push(ic("Resume", "resume", playSvg));
  if (status === "err")  out.push(ic("Retry", "restart", retrySvg));
  if (status === "done") out.push(ic("Open file", "openFile", fileSvg));
  out.push(ic("Show in folder", "openFolder", folderSvg));
  out.push(ic("Remove", "remove", trashSvg));
  return out.join("");
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
}

function renderAll() {
  const rows = $("rows");
  // Remove rows whose ids no longer exist or no longer match the filter.
  const keep = new Set();
  for (const item of state.items.values()) {
    if (rowMatchesFilter(item)) keep.add(String(item.id));
  }
  for (const node of [...rows.querySelectorAll(".row")]) {
    if (!keep.has(node.dataset.id)) node.remove();
  }
  // Render/update rows that should exist.
  // Sort: running first, then queued, then most-recent done.
  const visible = [...state.items.values()]
    .filter(rowMatchesFilter)
    .sort((a, b) => stateOrder(a) - stateOrder(b) || (b.startedEpoch||0) - (a.startedEpoch||0));
  for (const item of visible) renderRow(item);
  // Reorder the DOM to match `visible`.
  for (const item of visible) {
    const node = rows.querySelector(`.row[data-id="${item.id}"]`);
    if (node) rows.appendChild(node);
  }
  $("emptyState").classList.toggle("hidden", state.items.size > 0);
  refreshCounts();
}

function stateOrder(item) {
  switch (statusOf(item)) {
    case "running": return 0;
    case "queued":  return 1;
    case "err":     return 2;
    case "aborted": return 3;
    case "done":    return 4;
  }
  return 5;
}

function refreshCounts() {
  const c = { all: 0, running: 0, queued: 0, done: 0, err: 0,
              compressed: 0, documents: 0, music: 0, programs: 0, video: 0 };
  for (const item of state.items.values()) {
    c.all++;
    const st = statusOf(item);
    if (c[st] !== undefined) c[st]++;
    const k = kindOf(item.filename, item.url);
    if      (k === "archive")  c.compressed++;
    else if (k === "document") c.documents++;
    else if (k === "audio")    c.music++;
    else if (k === "program")  c.programs++;
    else if (k === "video")    c.video++;
  }
  for (const [k, v] of Object.entries(c)) {
    const el = $("cnt" + k.charAt(0).toUpperCase() + k.slice(1));
    if (el) el.textContent = String(v);
  }
}

// ---- inbound handlers ----------------------------------------------

function onItems(list) {
  state.items.clear();
  for (const it of list || []) state.items.set(it.id, it);
  renderAll();
}

function onItem(item) {
  if (!item || item.id == null) return;
  state.items.set(item.id, item);
  renderAll();
}

function onRemove(id) {
  state.items.delete(id);
  renderAll();
}

function onSettings(settings) {
  state.settings = settings || {};
}

function onWindowState(ev) {
  // Optional: hint for max/restore button toggling.
  document.body.classList.toggle("maximized", !!ev.maximized);
}

// ---- toasts ---------------------------------------------------------

function toast(message, kind) {
  const t = document.createElement("div");
  t.className = "toast " + (kind || "info");
  t.textContent = message;
  $("toasts").appendChild(t);
  setTimeout(() => {
    t.style.transition = "opacity 200ms";
    t.style.opacity = "0";
    setTimeout(() => t.remove(), 250);
  }, 3500);
}

// ---- modals ---------------------------------------------------------

function openModal(id) {
  const m = $(id);
  if (!m) return;
  m.hidden = false;
  if (id === "addModal") {
    setTimeout(() => $("addUrl").focus(), 0);
    // Try to prefill from clipboard if it looks like a URL.
    if (navigator.clipboard && navigator.clipboard.readText) {
      navigator.clipboard.readText().then(t => {
        if (!$("addUrl").value && /^(https?|ftp|ftps):\/\//i.test(t.trim())) {
          $("addUrl").value = t.trim();
        }
      }).catch(() => {});
    }
  } else if (id === "settingsModal") {
    fillSettings();
  }
}

function closeModal(id) { const m = $(id); if (m) m.hidden = true; }

// ---- properties dialog ---------------------------------------------

const propsState = { id: null };

function openProperties(id) {
  const item = state.items.get(id);
  if (!item) return;
  propsState.id = id;
  const kind = kindOf(item.filename, item.url);
  const typeMap = {
    archive:  "Archive",
    video:    "Video",
    audio:    "Audio",
    program:  "Application",
    document: "Document",
    file:     "File"
  };
  $("propType").textContent     = typeMap[kind] || "File";
  $("propStatus").textContent   = statusOf(item);
  let sizeText = "—";
  if (item.total > 0) {
    sizeText = `${fmtBytes(item.total)} (${item.total.toLocaleString()} bytes)`;
  } else if (item.downloaded > 0) {
    sizeText = `${fmtBytes(item.downloaded)} downloaded (size unknown)`;
  }
  $("propSize").textContent = sizeText;
  $("propSaveTo").textContent  = item.resultPath || item.outDir || "—";
  $("propUrl").textContent     = item.url     || "—";
  $("propReferer").textContent = item.referer || "—";
  $("propUA").textContent      = item.userAgent || "—";
  $("propDesc").textContent    = item.message  || "—";
  // Hide Open buttons unless the download is finished.
  const isDone = statusOf(item) === "done";
  $("btnPropsOpen").style.display = isDone ? "" : "none";
  openModal("propsModal");
}

// ---- right-click context menu --------------------------------------

const ctxState = { id: null };

function showCtxMenu(id, x, y) {
  const m = $("ctxMenu");
  if (!m) return;
  ctxState.id = id;
  // Disable items that don't apply to current state.
  const item = state.items.get(id);
  const st   = item ? statusOf(item) : "";
  m.querySelectorAll(".ctx-item").forEach(el => {
    const a = el.dataset.act;
    let disabled = false;
    if (a === "resume" && st !== "queued" && st !== "aborted" && st !== "err") disabled = true;
    if (a === "pause"  && st !== "running") disabled = true;
    if (a === "openFile" && st !== "done") disabled = true;
    el.classList.toggle("disabled", disabled);
  });
  m.hidden = false;
  // Position, clamping to viewport.
  const r = m.getBoundingClientRect();
  const vw = window.innerWidth, vh = window.innerHeight;
  if (x + r.width  > vw) x = vw - r.width  - 4;
  if (y + r.height > vh) y = vh - r.height - 4;
  m.style.left = x + "px";
  m.style.top  = y + "px";
}

function hideCtxMenu() {
  const m = $("ctxMenu");
  if (m) m.hidden = true;
  ctxState.id = null;
}

// ---- batch download URL expansion ----------------------------------

// Expand `[N-M]` (numeric, optional zero-pad) and `[a-z]`/`[A-Z]` (alpha)
// patterns inside a URL into the full list of URLs they represent.
// Multiple ranges expand as a Cartesian product. Pure literal URLs pass
// through unchanged. Anything that doesn't parse as a URL is dropped.
function expandPattern(url) {
  const re = /\[([^\]]+)\]/;
  const m = url.match(re);
  if (!m) return [url];
  const inner = m[1];
  let values = null;

  // Numeric range: 1-100, 001-100 (zero-pad to first operand's width).
  const numM = inner.match(/^(\d+)-(\d+)$/);
  if (numM) {
    const a = parseInt(numM[1], 10);
    const b = parseInt(numM[2], 10);
    if (a <= b && b - a < 10000) {
      const width = numM[1].length === numM[2].length ? numM[1].length : 0;
      values = [];
      for (let i = a; i <= b; ++i) {
        values.push(width ? String(i).padStart(width, "0") : String(i));
      }
    }
  }
  // Alpha range: a-z or A-Z (single chars).
  const alphaM = !values && inner.match(/^([a-z])-([a-z])$/i);
  if (alphaM) {
    const a = alphaM[1].charCodeAt(0);
    const b = alphaM[2].charCodeAt(0);
    if (a <= b) {
      values = [];
      for (let c = a; c <= b; ++c) values.push(String.fromCharCode(c));
    }
  }
  if (!values) return [url];

  const before = url.slice(0, m.index);
  const after  = url.slice(m.index + m[0].length);
  const out = [];
  for (const v of values) {
    // Recurse for additional ranges.
    for (const expanded of expandPattern(before + v + after)) out.push(expanded);
  }
  return out;
}

function expandBatch(text) {
  const lines = (text || "").split(/\r?\n/).map(s => s.trim()).filter(Boolean);
  const out = [];
  const seen = new Set();
  for (const line of lines) {
    if (!/^(https?|ftp|ftps):\/\//i.test(line)) continue;
    for (const u of expandPattern(line)) {
      if (seen.has(u)) continue;
      seen.add(u);
      out.push(u);
    }
  }
  return out;
}

function refreshBatchPreview() {
  const urls = expandBatch($("batchUrls").value);
  const el = $("batchPreview");
  if (!el) return;
  if (urls.length === 0) {
    el.textContent = "0 URLs queued";
  } else if (urls.length <= 3) {
    el.textContent = `${urls.length} URL${urls.length === 1 ? "" : "s"} ready: ${urls.join(", ")}`;
  } else {
    el.textContent = `${urls.length} URLs ready (first: ${urls[0]}, last: ${urls[urls.length-1]})`;
  }
}

function fillSettings() {
  const s = state.settings || {};
  $("setOutDir").value          = s.outDir          || "";
  $("setConns").value            = s.connections    || 8;
  $("setMaxJobs").value          = s.maxJobs        || 3;
  $("setBwKbps").value            = Math.floor((s.bandwidthBps || 0) / 1024);
  $("setClipWatch").checked       = !!s.clipboardWatch;
  $("setVerifyChecksum").checked  = s.verifyChecksum !== false;
}

// ---- event wiring ---------------------------------------------------

document.addEventListener("DOMContentLoaded", () => {
  // Sidebar filters
  document.querySelectorAll(".cat-item").forEach(el => {
    el.addEventListener("click", () => {
      document.querySelectorAll(".cat-item").forEach(e => e.classList.remove("active"));
      el.classList.add("active");
      state.filter = el.dataset.cat;
      renderAll();
    });
  });

  // Search
  $("search").addEventListener("input", e => {
    state.search = e.target.value.trim().toLowerCase();
    renderAll();
  });

  // Toolbar
  $("btnAdd").addEventListener("click",      () => openModal("addModal"));
  $("btnBatch").addEventListener("click",    () => openModal("batchModal"));
  $("btnSettings").addEventListener("click", () => openModal("settingsModal"));
  $("btnPauseAll").addEventListener("click", () => send({type:"pauseAll"}));
  $("btnResumeAll").addEventListener("click",() => send({type:"resumeAll"}));

  // Window controls
  $("winMin").addEventListener("click",   () => send({type:"win.minimize"}));
  $("winMax").addEventListener("click",   () => send({type:"win.toggleMaximize"}));
  $("winClose").addEventListener("click", () => send({type:"win.close"}));

  // Modal closes
  document.querySelectorAll("[data-close]").forEach(el => {
    el.addEventListener("click", () => closeModal(el.dataset.close));
  });

  // Add download
  $("btnAddOk").addEventListener("click", () => {
    const url = $("addUrl").value.trim();
    if (!url) return;
    send({
      type: "addDownload",
      url,
      filename: $("addName").value.trim(),
      outDir:   $("addOutDir").value.trim()
    });
    $("addUrl").value = "";
    $("addName").value = "";
    closeModal("addModal");
  });

  // Batch download — live preview of expanded URLs.
  $("batchUrls").addEventListener("input", refreshBatchPreview);
  $("btnBatchOk").addEventListener("click", () => {
    const urls = expandBatch($("batchUrls").value);
    if (urls.length === 0) {
      toast("No URLs to add", "err");
      return;
    }
    if (urls.length > 500 && !confirm(`Add ${urls.length} downloads?`)) return;
    const outDir = $("batchOutDir").value.trim();
    for (const u of urls) {
      send({ type: "addDownload", url: u, outDir });
    }
    toast(`Queued ${urls.length} download${urls.length === 1 ? "" : "s"}`, "ok");
    $("batchUrls").value = "";
    refreshBatchPreview();
    closeModal("batchModal");
  });

  // Settings save
  $("btnSettingsSave").addEventListener("click", () => {
    const s = {
      outDir:          $("setOutDir").value.trim(),
      connections:     parseInt($("setConns").value, 10) || 8,
      maxJobs:         parseInt($("setMaxJobs").value, 10) || 3,
      bandwidthBps:    (parseInt($("setBwKbps").value, 10) || 0) * 1024,
      clipboardWatch:  $("setClipWatch").checked,
      verifyChecksum:  $("setVerifyChecksum").checked
    };
    send({type:"setSettings", settings: s});
    state.settings = s;
    closeModal("settingsModal");
  });

  // Per-row actions (event delegation)
  $("rows").addEventListener("click", (e) => {
    const btn = e.target.closest(".row-action");
    if (!btn) return;
    const row = e.target.closest(".row");
    const id  = parseInt(row.dataset.id, 10);
    const act = btn.dataset.act;
    send({type: act, id});
  });

  // Double-click row → Properties modal (IDM-parity).
  $("rows").addEventListener("dblclick", (e) => {
    if (e.target.closest(".row-action")) return; // don't conflict with action buttons
    const row = e.target.closest(".row");
    if (!row) return;
    const id = parseInt(row.dataset.id, 10);
    openProperties(id);
  });

  // Right-click → context menu.
  $("rows").addEventListener("contextmenu", (e) => {
    const row = e.target.closest(".row");
    if (!row) return;
    e.preventDefault();
    const id = parseInt(row.dataset.id, 10);
    showCtxMenu(id, e.clientX, e.clientY);
  });

  document.addEventListener("click", (e) => {
    if (!e.target.closest("#ctxMenu")) hideCtxMenu();
  });
  document.addEventListener("scroll", hideCtxMenu, true);
  window.addEventListener("blur", hideCtxMenu);

  $("ctxMenu").addEventListener("click", (e) => {
    const it = e.target.closest(".ctx-item");
    if (!it) return;
    const act = it.dataset.act;
    const id  = ctxState.id;
    hideCtxMenu();
    if (id == null) return;
    if (act === "properties") openProperties(id);
    else send({type: act, id});
  });

  $("btnPropsOpen").addEventListener("click", () => {
    if (propsState.id != null) send({type: "openFile", id: propsState.id});
  });
  $("btnPropsFolder").addEventListener("click", () => {
    if (propsState.id != null) send({type: "openFolder", id: propsState.id});
  });

  // Drag & drop URLs onto window
  document.addEventListener("dragover", e => { e.preventDefault(); });
  document.addEventListener("drop", e => {
    e.preventDefault();
    const text = e.dataTransfer.getData("text/uri-list") || e.dataTransfer.getData("text/plain");
    if (!text) return;
    text.split(/\r?\n/).map(s => s.trim()).filter(Boolean).forEach(u => {
      if (/^(https?|ftp|ftps):\/\//i.test(u)) send({type: "addDownload", url: u});
    });
  });

  // Keyboard
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      document.querySelectorAll(".modal-backdrop:not([hidden])").forEach(m => m.hidden = true);
    }
    if ((e.ctrlKey || e.metaKey) && e.key === "n") {
      e.preventDefault();
      openModal("addModal");
    }
  });

  // Tell the host we're ready and it can blast initial state.
  send({type: "ready"});
});

})();
