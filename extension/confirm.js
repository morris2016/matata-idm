// Confirm dialog for intercepted downloads.
//
// Opened by background.js via chrome.windows.create when a download matches
// the interceptor filters. Pulls pending details by key, presents Start /
// Download later / Cancel, and posts the choice back to background.

const params = new URLSearchParams(location.search);
const key = params.get("id") || "";

function $(id) { return document.getElementById(id); }

function fmtBytes(n) {
  if (!n || n <= 0) return "unknown";
  const u = ["B","KiB","MiB","GiB","TiB"];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
  return (i === 0 ? v : v.toFixed(2)) + " " + u[i];
}

function extOf(name) {
  if (!name) return "";
  const m = name.match(/\.([A-Za-z0-9]+)$/);
  return m ? m[1].toLowerCase() : "";
}

function host(url) {
  try { return new URL(url).host; } catch { return ""; }
}

let pending = null;

function send(msg) {
  return new Promise((resolve) => chrome.runtime.sendMessage(msg, resolve));
}

async function load() {
  const r = await send({ action: "get-pending", key });
  if (!r || !r.ok || !r.item) {
    $("filename").textContent = "(request expired)";
    $("btnStart").disabled = true;
    $("btnLater").disabled = true;
    return;
  }
  pending = r.item;
  const ext = extOf(pending.filename) || extOf((pending.url || "").split("?")[0]);
  $("filename").textContent = pending.filename || "(unknown name)";
  $("size").textContent = fmtBytes(pending.fileSize);
  $("url").textContent = pending.url || "";
  $("url").title = pending.url || "";
  $("referer").textContent = host(pending.referer) || pending.referer || "(none)";
  $("referer").title = pending.referer || "";
  $("outDir").textContent = pending.outDir || "Downloads (default)";
  $("extLabel").textContent = ext ? "." + ext + " files" : "this file type";
  $("dontAskExt").dataset.ext = ext;
}

async function choose(choice) {
  $("btnStart").disabled = true;
  $("btnLater").disabled = true;
  $("btnCancel").disabled = true;
  const resp = await send({
    action: "confirm-pending",
    key,
    choice,
    dontAskExt: $("dontAskExt").checked ? ($("dontAskExt").dataset.ext || "") : ""
  });
  if (choice !== "cancel" && (!resp || !resp.ok)) {
    showError(resp && resp.error ? String(resp.error) : "no response from matata-host");
    return;
  }
  window.close();
}

function showError(text) {
  let bar = document.getElementById("errBar");
  if (!bar) {
    bar = document.createElement("div");
    bar.id = "errBar";
    bar.style.cssText =
      "margin:8px 0;padding:8px 10px;border-radius:3px;" +
      "background:#fde7e7;border:1px solid #b30000;color:#b30000;font-size:12px;";
    document.querySelector(".box").after(bar);
  }
  bar.textContent = "matata error: " + text;
  $("btnStart").disabled = false;
  $("btnLater").disabled = false;
  $("btnCancel").disabled = false;
  pending = null; // disarm beforeunload cancel
}

document.addEventListener("DOMContentLoaded", () => {
  load();
  $("btnStart").addEventListener("click",  () => choose("start"));
  $("btnLater").addEventListener("click",  () => choose("later"));
  $("btnCancel").addEventListener("click", () => choose("cancel"));
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") choose("cancel");
  });
});

// If the user closes the window via the [x], count that as cancel.
window.addEventListener("beforeunload", () => {
  if (!pending) return;
  try { chrome.runtime.sendMessage({ action: "confirm-pending", key, choice: "cancel" }); } catch {}
});
