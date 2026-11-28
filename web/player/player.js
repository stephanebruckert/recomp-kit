import { queryEnvironment, executablePath } from "./env.mjs?v=__RECOMP_BUILD__";
import { selectSource } from "./source.mjs?v=__RECOMP_BUILD__";
"use strict";
const statusEl = document.getElementById("status");
const canvas = document.getElementById("canvas");
function status(text, back) {
  statusEl.textContent = text;
  if (back) {
    const a = document.createElement("a");
    a.href = "../";
    a.textContent = "\nBack to the launcher";
    statusEl.append(document.createElement("br"), a);
  }
  statusEl.hidden = !text;
}
const log = [];
window.recompLog = log; // for tests
function record(text, warning) {
  log.push(text);
  if (log.length > 2000) log.shift();
  if (text.startsWith("[stream] failed:"))
    status("The hosted game files could not be loaded. Return to the launcher and retry or import your copy.", true);
  (warning ? console.warn : console.log)(text);
}

async function gameId() {
  const parts = location.pathname.split("/").filter(Boolean);
  if (parts.length && parts[parts.length - 1].includes(".")) parts.pop();
  return decodeURIComponent(parts[parts.length - 1] || "");
}

async function start() {
  if (!crossOriginIsolated) throw new Error("This page must be served cross-origin isolated (COOP/COEP headers).");
  if (!navigator.gpu) throw new Error("This browser has no WebGPU.");
  const pathId = await gameId();
  const gamesUrl = new URL("../games.json", location.href).href;
  const games = await fetch(gamesUrl).then((r) => r.json());
  // At a site's root there is no game folder in the address: a site that
  // lists a single game plays that one.
  const game = pathId ? games.find((g) => g.id === pathId) : games.length === 1 ? games[0] : undefined;
  if (!game) throw new Error(`No game "${pathId}" in the launcher's games.json.`);
  const id = game.id;
  document.title = game.title;

  // Imported data remains preferred by default. Explicit streaming never
  // modifies that installation, and both modes share the same OPFS profile.
  let imported = false;
  let dir = await navigator.storage.getDirectory();
  try {
    for (const p of [id, "game"]) dir = await dir.getDirectoryHandle(p);
    const components = executablePath(game).split("/");
    const file = components.pop();
    for (const part of components) dir = await dir.getDirectoryHandle(part);
    await dir.getFileHandle(file);
    imported = (await (await dirForStamp(id)).text()).trim() === game.sha256;
  } catch {}
  const source = await selectSource(game, gamesUrl, location.search, imported);
  window.recompSource = source.kind;
  // Streamed data is kept in this origin's storage; ask the browser not to
  // evict it under storage pressure, as it would the cache of a normal page.
  if (source.kind === "stream" && navigator.storage.persist) navigator.storage.persist().catch(() => {});

  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("WebGPU gave no adapter.");
  const wanted = ["texture-compression-bc", "depth32float-stencil8", "float32-filterable",
                  "unorm16-texture-formats", "rg11b10ufloat-renderable"];
  const requiredFeatures = wanted.filter((f) => adapter.features.has(f));
  const requiredLimits = {};
  for (const k of ["maxTextureDimension2D", "maxBufferSize", "maxStorageBufferBindingSize",
                   "maxUniformBufferBindingSize", "maxBindGroups", "maxSampledTexturesPerShaderStage",
                   "maxSamplersPerShaderStage", "maxStorageBuffersPerShaderStage",
                   "maxUniformBuffersPerShaderStage", "maxDynamicUniformBuffersPerPipelineLayout",
                   "maxVertexBuffers", "maxVertexAttributes", "maxColorAttachments",
                   "maxInterStageShaderVariables"])
    if (k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  const device = await adapter.requestDevice({ requiredFeatures, requiredLimits });
  device.lost.then((info) => status(`The graphics device was lost: ${info.message}`, true));
  device.addEventListener("uncapturederror", (e) => console.error("WebGPU:", e.error.message));

  const Module = {
    canvas,
    preinitializedWebGPUDevice: device,
    noInitialRun: true,
    // The build stamp keeps a cached module, Wasm or data file from an older
    // build out of this one (tools/web_launcher.py fills it in when staging).
    locateFile: (path, prefix) => `${prefix}${path}?v=__RECOMP_BUILD__`,
    print: (t) => record(t, false),
    printErr: (t) => record(t, true),
    setStatus: (t) => status(t ? t : ""),
    onAbort: (what) => status(`${document.title} has crashed: ${what}`, true),
    // The game's thread ended. Quitting from the game's own menu returns to
    // the launcher; a game that stopped any other way says so and stays on
    // this page with the last error it logged: "could not start" before its
    // window was shown, "has crashed" after.
    onGameExit(code) {
      if (code === 0) {
        status("The game has quit. Returning to the launcher…");
        location.assign("../");
        return;
      }
      const last = [...log].reverse().find((t) => /failed|could not|unavailable|missing|error|fatal/i.test(t) && !/run record/.test(t));
      const started = log.some((t) => /window [0-9a-f]+ shown/.test(t));
      if (!statusEl.querySelector("a"))
        status(started
          ? `${document.title} has crashed (exit code ${code})${last ? `: ${last}` : "."}`
          : `The game could not start${last ? `: ${last}` : "."}`, true);
    },
    preRun: [() => {
      const m = Module;
      m.ENV.RECOMP_PROFILE_DIR = `/opfs/${id}/profile`;
      // TouchMapper places the guest cursor before emitting its click. SDL's
      // additional synthetic mouse click would fire first at the old cursor.
      m.ENV.SDL_TOUCH_MOUSE_EVENTS = "0";
      Object.assign(m.ENV, queryEnvironment(location.search));
    }],
    onRuntimeInitialized() {
      status("");
      canvas.focus();
      Module.callMain(source.args);
    },
  };
  window.Module = Module;
  const script = document.createElement("script");
  script.src = "@APP@.js?v=__RECOMP_BUILD__";
  script.onerror = () => status("The game's web build is missing.", true);
  document.body.append(script);
}

async function dirForStamp(id) {
  const root = await navigator.storage.getDirectory();
  const game = await (await root.getDirectoryHandle(id)).getDirectoryHandle("game");
  return (await game.getFileHandle(".stamp")).getFile();
}

// A streamed game keeps what it downloads in this browser and fills in the
// rest in the background: a bar shows how much of the whole game is saved
// locally (bytes, %, download speed) until all of it is, and says so once.
if (typeof BroadcastChannel === "function") {
  const box = document.getElementById("stream");
  const label = document.getElementById("stream-label");
  const fill = document.getElementById("stream-fill");
  const mb = (b) => (b / 1048576).toFixed(0);
  let last = null, waiting = false, waitRate = 0, rate = 0, prev = null, hideTimer = 0;
  const render = () => {
    if (!last || !last.total) return;
    const done = last.filled >= last.total;
    const pct = Math.min(100, Math.floor(100 * last.filled / last.total));
    box.hidden = false;
    box.classList.toggle("done", done);
    box.classList.toggle("waiting", waiting && !done);
    fill.style.width = `${pct}%`;
    const speed = waiting ? waitRate : rate;
    label.textContent = done
      ? `Game saved to this browser (${mb(last.total)} MB): plays offline`
      : `${waiting ? "Loading game data" : "Downloading the game"}: ${pct}% · ` +
        `${mb(last.filled)} / ${mb(last.total)} MB` + (speed > 0 ? ` · ${(speed / 1e6).toFixed(1)} MB/s` : "");
    clearTimeout(hideTimer);
    if (done) hideTimer = setTimeout(() => { box.hidden = true; }, 4000);
  };
  new BroadcastChannel("recomp-stream").onmessage = ({ data }) => {
    if (!data) return;
    if ("waiting" in data) {
      waiting = data.waiting;
      waitRate = data.rate || 0;
      render();
      return;
    }
    if (!data.total || !data.cached) return;
    const now = performance.now();
    if (prev && now > prev.t) rate = Math.max(0, (data.network - prev.network) * 1000 / (now - prev.t));
    prev = { t: now, network: data.network };
    const first = !last;
    last = data;
    // Already complete when the page opened: nothing to show.
    if (first && data.filled >= data.total) return;
    render();
  };
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("click", () => canvas.focus());
start().catch((e) => status(e.message, true));
