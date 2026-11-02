/**
 * PiMix Frontend Controller (ui/frontend/app.js)
 * - Real-time WebSocket state, 25 FPS VU meters & 72-band FFT Spectrum Analyzer (RTA)
 * - Native <input type="range"> CSS custom property sync (--attr-min, --attr-max, --control-value)
 * - Interactive RBJ Audio EQ Cookbook Biquad Frequency Response + Live Pre/Post-EQ RTA Canvas
 */

const SAMPLE_RATE = 48000;
const NUM_SPEC_BINS = 72;
const BAND_COLORS = [
  "#22d3ee", "#38bdf8", "#a855f7", "#f59e0b", "#22c55e",
  "#ec4899", "#eab308", "#14b8a6", "#6366f1", "#f97316",
];

let appState = null;
let selectedEqTarget = "master"; // "usb1".."bt" or "master"
let ws = null;
let sendThrottleTimer = null;
let pendingWsPayload = null;
let activeDragBandIdx = -1;

// Real-Time Spectrum Analyzer (RTA) State
let rtaMode = "pre_post"; // "pre_post" | "post" | "off"
let rtaPeakHoldEnabled = true;
let spectrumFreqs = Array.from({ length: NUM_SPEC_BINS }, (_, i) =>
  20 * Math.pow(1000, (i + 0.5) / NUM_SPEC_BINS)
);
let rawSpectrumByTarget = {
  usb1: new Array(NUM_SPEC_BINS).fill(-95),
  usb2: new Array(NUM_SPEC_BINS).fill(-95),
  master: new Array(NUM_SPEC_BINS).fill(-95),
};
let smoothPreSpec = new Float32Array(NUM_SPEC_BINS).fill(-95);
let smoothPostSpec = new Float32Array(NUM_SPEC_BINS).fill(-95);
let peakHoldSpec = new Float32Array(NUM_SPEC_BINS).fill(-95);
let peakHoldAge = new Uint16Array(NUM_SPEC_BINS).fill(0);

const stripsContainer = document.getElementById("channel-strips-container");
const eqBandsContainer = document.getElementById("eq-bands-container");
const eqCanvas = document.getElementById("eq-canvas");
const eqCtx = eqCanvas.getContext("2d");

// Sync CSS custom properties on native <input type="range"> per modern-web-guidance `slider`
function syncSliderValue(slider, formattedText) {
  slider.style.setProperty("--attr-min", slider.min || 0);
  slider.style.setProperty("--attr-max", slider.max || 100);
  slider.style.setProperty("--control-value", slider.value);
  if (formattedText) {
    slider.setAttribute("aria-valuetext", formattedText);
  }
}

function formatDb(val) {
  const num = Number(val);
  if (num <= -59.5) return "-∞ dB";
  return `${num >= 0 ? "+" : ""}${num.toFixed(1)} dB`;
}

function formatPan(val) {
  const num = Math.round(Number(val) * 100);
  if (Math.abs(num) <= 2) return "Center";
  return num < 0 ? `L${Math.abs(num)}` : `R${num}`;
}

function formatFreq(hz) {
  const num = Number(hz);
  return num >= 1000 ? `${(num / 1000).toFixed(num >= 10000 ? 1 : 2)} kHz` : `${Math.round(num)} Hz`;
}

function dbToMeterPercent(db) {
  if (db <= -60) return 0;
  if (db >= 0) return 100;
  return ((db + 60) / 60) * 100;
}

function sendWsThrottled(payload) {
  pendingWsPayload = payload;
  if (sendThrottleTimer) return;
  sendThrottleTimer = setTimeout(() => {
    sendThrottleTimer = null;
    if (pendingWsPayload && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(pendingWsPayload));
      pendingWsPayload = null;
    }
  }, 35);
}

function sendWsImmediate(payload) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(payload));
  }
}

// ---------------------------------------------------------------------------
// Render Channel Strips + Master Strip
// ---------------------------------------------------------------------------
function renderStrips() {
  if (!appState) return;
  stripsContainer.innerHTML = "";

  appState.channels.forEach((ch, idx) => {
    const strip = document.createElement("article");
    strip.className = `channel-strip${selectedEqTarget === ch.id ? " selected-for-eq" : ""}`;
    strip.dataset.stripId = ch.id;

    const gainId = `fader-${ch.id}`;
    const panId = `pan-${ch.id}`;
    const gainText = formatDb(ch.gain_db);
    const panText = formatPan(ch.pan);

    strip.innerHTML = `
      <div class="strip-header">
        <div>
          <div class="strip-title">${ch.name}</div>
          <div class="strip-sub">${ch.subtitle || ""}</div>
        </div>
        <span class="hw-badge ${ch.active_hw ? "" : "standby"}">
          ${ch.active_hw ? "LIVE I2S" : "STBY"}
        </span>
      </div>

      <div class="vu-meter-box" aria-label="${ch.name} Stereo Level Meter">
        <div class="vu-row">
          <span class="vu-ch-label">L</span>
          <div class="vu-track"><div class="vu-fill" id="vu-l-${idx}"></div></div>
        </div>
        <div class="vu-row">
          <span class="vu-ch-label">R</span>
          <div class="vu-track"><div class="vu-fill" id="vu-r-${idx}"></div></div>
        </div>
        <div class="vu-readout">
          <span>Peak</span>
          <span id="vu-db-${idx}">-∞ dBFS</span>
        </div>
      </div>

      <div class="strip-buttons">
        <button type="button" class="strip-btn btn-mute" aria-pressed="${Boolean(ch.mute)}" aria-label="Mute ${ch.name}">M</button>
        <button type="button" class="strip-btn btn-solo" aria-pressed="${Boolean(ch.solo)}" aria-label="Solo ${ch.name}">S</button>
        <button type="button" class="strip-btn btn-invert" aria-pressed="${Boolean(ch.invert)}" aria-label="Invert Polarity ${ch.name}">Ø</button>
        <button type="button" class="strip-btn btn-eq-focus ${selectedEqTarget === ch.id ? "active" : ""}" aria-label="Edit EQ for ${ch.name}">EQ</button>
      </div>

      <div class="control-group">
        <div class="control-label-row">
          <label for="${gainId}">Fader Gain</label>
          <output id="${gainId}-out" for="${gainId}">${gainText}</output>
        </div>
        <input type="range" id="${gainId}" name="${gainId}" min="-60" max="12" step="0.5" value="${ch.gain_db}">
      </div>

      <div class="control-group">
        <div class="control-label-row">
          <label for="${panId}">Balance</label>
          <output id="${panId}-out" for="${panId}">${panText}</output>
        </div>
        <input type="range" id="${panId}" name="${panId}" min="-1" max="1" step="0.02" value="${ch.pan}">
      </div>
    `;

    const gainSlider = strip.querySelector(`#${gainId}`);
    const gainOut = strip.querySelector(`#${gainId}-out`);
    syncSliderValue(gainSlider, gainText);
    gainSlider.addEventListener("input", () => {
      ch.gain_db = parseFloat(gainSlider.value);
      const txt = formatDb(ch.gain_db);
      gainOut.textContent = txt;
      syncSliderValue(gainSlider, txt);
      sendWsThrottled({ type: "update_channel", id: ch.id, gain_db: ch.gain_db });
    });

    const panSlider = strip.querySelector(`#${panId}`);
    const panOut = strip.querySelector(`#${panId}-out`);
    syncSliderValue(panSlider, panText);
    panSlider.addEventListener("input", () => {
      ch.pan = parseFloat(panSlider.value);
      const txt = formatPan(ch.pan);
      panOut.textContent = txt;
      syncSliderValue(panSlider, txt);
      sendWsThrottled({ type: "update_channel", id: ch.id, pan: ch.pan });
    });
    panSlider.addEventListener("dblclick", () => {
      ch.pan = 0;
      panSlider.value = "0";
      const txt = formatPan(0);
      panOut.textContent = txt;
      syncSliderValue(panSlider, txt);
      sendWsImmediate({ type: "update_channel", id: ch.id, pan: 0 });
    });

    strip.querySelector(".btn-mute").addEventListener("click", (e) => {
      ch.mute = !ch.mute;
      e.currentTarget.setAttribute("aria-pressed", String(ch.mute));
      sendWsImmediate({ type: "update_channel", id: ch.id, mute: ch.mute });
    });

    strip.querySelector(".btn-solo").addEventListener("click", (e) => {
      ch.solo = !ch.solo;
      e.currentTarget.setAttribute("aria-pressed", String(ch.solo));
      sendWsImmediate({ type: "update_channel", id: ch.id, solo: ch.solo });
    });

    strip.querySelector(".btn-invert").addEventListener("click", (e) => {
      ch.invert = !ch.invert;
      e.currentTarget.setAttribute("aria-pressed", String(ch.invert));
      sendWsImmediate({ type: "update_channel", id: ch.id, invert: ch.invert });
    });

    strip.querySelector(".btn-eq-focus").addEventListener("click", () => {
      selectEqTarget(ch.id);
    });

    stripsContainer.appendChild(strip);
  });

  // Render Master Stereo Output Strip
  const m = appState.master;
  const mStrip = document.createElement("article");
  mStrip.className = `channel-strip master-strip${selectedEqTarget === "master" ? " selected-for-eq" : ""}`;
  const mGainText = formatDb(m.gain_db);
  const hwVolText = `${m.hw_volume_pct ?? 75}%`;

  mStrip.innerHTML = `
    <div class="strip-header">
      <div>
        <div class="strip-title">Master Out</div>
        <div class="strip-sub">USB DAC • Stereo</div>
      </div>
      <span class="hw-badge">OUT</span>
    </div>

    <div class="vu-meter-box" aria-label="Master Stereo Output Level Meter">
      <div class="vu-row">
        <span class="vu-ch-label">L</span>
        <div class="vu-track"><div class="vu-fill" id="vu-master-l"></div></div>
      </div>
      <div class="vu-row">
        <span class="vu-ch-label">R</span>
        <div class="vu-track"><div class="vu-fill" id="vu-master-r"></div></div>
      </div>
      <div class="vu-readout">
        <span>Master Peak</span>
        <span id="vu-master-db">-∞ dBFS</span>
      </div>
    </div>

    <div class="strip-buttons" style="grid-template-columns: 1fr 1fr 1fr;">
      <button type="button" class="strip-btn btn-mute" id="btn-master-mute" aria-pressed="${Boolean(m.mute)}" aria-label="Mute Master Output">MUTE</button>
      <button type="button" class="strip-btn btn-invert" id="btn-master-lim" aria-pressed="${Boolean(m.limiter_enabled)}" aria-label="Toggle Master Peak Limiter">LIM</button>
      <button type="button" class="strip-btn btn-eq-focus ${selectedEqTarget === "master" ? "active" : ""}" id="btn-master-eq" aria-label="Edit Master 10-Band EQ">10-EQ</button>
    </div>

    <div class="control-group">
      <div class="control-label-row">
        <label for="fader-master">Master Trim</label>
        <output id="fader-master-out" for="fader-master">${mGainText}</output>
      </div>
      <input type="range" id="fader-master" name="fader-master" min="-60" max="12" step="0.5" value="${m.gain_db}">
    </div>

    <div class="control-group">
      <div class="control-label-row">
        <label for="fader-hw-vol">Speaker HW Vol</label>
        <output id="fader-hw-vol-out" for="fader-hw-vol">${hwVolText}</output>
      </div>
      <input type="range" id="fader-hw-vol" name="fader-hw-vol" min="0" max="100" step="1" value="${m.hw_volume_pct ?? 75}">
    </div>
  `;

  const mGainSlider = mStrip.querySelector("#fader-master");
  const mGainOut = mStrip.querySelector("#fader-master-out");
  syncSliderValue(mGainSlider, mGainText);
  mGainSlider.addEventListener("input", () => {
    m.gain_db = parseFloat(mGainSlider.value);
    const txt = formatDb(m.gain_db);
    mGainOut.textContent = txt;
    syncSliderValue(mGainSlider, txt);
    sendWsThrottled({ type: "update_master", gain_db: m.gain_db });
  });

  const hwSlider = mStrip.querySelector("#fader-hw-vol");
  const hwOut = mStrip.querySelector("#fader-hw-vol-out");
  syncSliderValue(hwSlider, hwVolText);
  hwSlider.addEventListener("input", () => {
    m.hw_volume_pct = parseInt(hwSlider.value, 10);
    const txt = `${m.hw_volume_pct}%`;
    hwOut.textContent = txt;
    syncSliderValue(hwSlider, txt);
    sendWsThrottled({ type: "update_master", hw_volume_pct: m.hw_volume_pct });
  });

  mStrip.querySelector("#btn-master-mute").addEventListener("click", (e) => {
    m.mute = !m.mute;
    e.currentTarget.setAttribute("aria-pressed", String(m.mute));
    sendWsImmediate({ type: "update_master", mute: m.mute });
  });

  mStrip.querySelector("#btn-master-lim").addEventListener("click", (e) => {
    m.limiter_enabled = !m.limiter_enabled;
    e.currentTarget.setAttribute("aria-pressed", String(m.limiter_enabled));
    sendWsImmediate({ type: "update_master", limiter_enabled: m.limiter_enabled });
  });

  mStrip.querySelector("#btn-master-eq").addEventListener("click", () => {
    selectEqTarget("master");
  });

  stripsContainer.appendChild(mStrip);
}

// ---------------------------------------------------------------------------
// EQ Target Selection & Band Controls
// ---------------------------------------------------------------------------
function getActiveEqObject() {
  if (!appState) return null;
  if (selectedEqTarget === "master") return appState.master;
  return appState.channels.find((c) => c.id === selectedEqTarget) || appState.master;
}

function selectEqTarget(targetId) {
  selectedEqTarget = targetId;
  // Reset RTA peak hold when switching channels so peaks reflect the newly selected target
  peakHoldSpec.fill(-95);
  smoothPreSpec.fill(-95);
  smoothPostSpec.fill(-95);
  document.querySelectorAll(".eq-select-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.eqTarget === targetId);
  });
  renderStrips();
  renderEqInspector();
}

function renderEqInspector() {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;

  const titleEl = document.getElementById("eq-target-title");
  if (selectedEqTarget === "master") {
    titleEl.textContent = "Master Output (10-Band Parametric/Graphic)";
  } else {
    titleEl.textContent = `${targetObj.name} (${targetObj.eq_bands.length}-Band Parametric)`;
  }

  const bypassBtn = document.getElementById("btn-eq-bypass");
  const isBypassed = Boolean(targetObj.eq_bypass);
  bypassBtn.setAttribute("aria-pressed", String(isBypassed));
  bypassBtn.textContent = isBypassed ? "EQ Bypassed" : "EQ Active";

  eqBandsContainer.innerHTML = "";
  targetObj.eq_bands.forEach((band, idx) => {
    const color = BAND_COLORS[idx % BAND_COLORS.length];
    const card = document.createElement("div");
    card.className = `eq-band-card${band.enabled === false ? " band-disabled" : ""}`;

    const gainId = `eq-gain-${idx}`;
    const freqId = `eq-freq-${idx}`;
    const qId = `eq-q-${idx}`;
    const gTxt = `${band.gain >= 0 ? "+" : ""}${Number(band.gain).toFixed(1)} dB`;
    const fTxt = formatFreq(band.freq);
    const qTxt = Number(band.q).toFixed(2);

    card.innerHTML = `
      <div class="eq-band-hdr">
        <span><span class="band-dot" style="background:${color}"></span>Band ${idx + 1} (${band.type})</span>
      </div>

      <div class="control-group">
        <div class="control-label-row">
          <label for="${gainId}">Gain</label>
          <output id="${gainId}-out" for="${gainId}">${gTxt}</output>
        </div>
        <input type="range" id="${gainId}" name="${gainId}" min="-18" max="18" step="0.5" value="${band.gain}" style="--slider-fill-color:${color}">
      </div>

      <div class="control-group">
        <div class="control-label-row">
          <label for="${freqId}">Freq</label>
          <output id="${freqId}-out" for="${freqId}">${fTxt}</output>
        </div>
        <input type="range" id="${freqId}" name="${freqId}" min="20" max="20000" step="1" value="${band.freq}" style="--slider-fill-color:${color}">
      </div>

      <div class="control-group">
        <div class="control-label-row">
          <label for="${qId}">Q Factor</label>
          <output id="${qId}-out" for="${qId}">${qTxt}</output>
        </div>
        <input type="range" id="${qId}" name="${qId}" min="0.3" max="8.0" step="0.05" value="${band.q}" style="--slider-fill-color:${color}">
      </div>
    `;

    const gSlider = card.querySelector(`#${gainId}`);
    const gOut = card.querySelector(`#${gainId}-out`);
    syncSliderValue(gSlider, gTxt);
    gSlider.addEventListener("input", () => {
      band.gain = parseFloat(gSlider.value);
      const txt = `${band.gain >= 0 ? "+" : ""}${band.gain.toFixed(1)} dB`;
      gOut.textContent = txt;
      syncSliderValue(gSlider, txt);
      drawEqCurve();
      pushEqUpdate();
    });

    const fSlider = card.querySelector(`#${freqId}`);
    const fOut = card.querySelector(`#${freqId}-out`);
    syncSliderValue(fSlider, fTxt);
    fSlider.addEventListener("input", () => {
      band.freq = parseFloat(fSlider.value);
      const txt = formatFreq(band.freq);
      fOut.textContent = txt;
      syncSliderValue(fSlider, txt);
      drawEqCurve();
      pushEqUpdate();
    });

    const qSlider = card.querySelector(`#${qId}`);
    const qOut = card.querySelector(`#${qId}-out`);
    syncSliderValue(qSlider, qTxt);
    qSlider.addEventListener("input", () => {
      band.q = parseFloat(qSlider.value);
      const txt = band.q.toFixed(2);
      qOut.textContent = txt;
      syncSliderValue(qSlider, txt);
      drawEqCurve();
      pushEqUpdate();
    });

    eqBandsContainer.appendChild(card);
  });

  drawEqCurve();
}

function pushEqUpdate() {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;
  if (selectedEqTarget === "master") {
    sendWsThrottled({
      type: "update_master",
      eq_bypass: targetObj.eq_bypass,
      eq_bands: targetObj.eq_bands,
    });
  } else {
    sendWsThrottled({
      type: "update_channel",
      id: selectedEqTarget,
      eq_bypass: targetObj.eq_bypass,
      eq_bands: targetObj.eq_bands,
    });
  }
}

// ---------------------------------------------------------------------------
// RBJ Audio EQ Cookbook Biquad Frequency Response & Live RTA Spectrum Graph
// ---------------------------------------------------------------------------
function biquadCoeffs(type, freq, gainDb, q) {
  const A = Math.pow(10, gainDb / 40);
  const w0 = (2 * Math.PI * freq) / SAMPLE_RATE;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Math.max(0.1, q));

  let b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  if (type === "Lowshelf") {
    const sqA = 2 * Math.sqrt(A) * alpha;
    b0 = A * ((A + 1) - (A - 1) * cosw0 + sqA);
    b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
    b2 = A * ((A + 1) - (A - 1) * cosw0 - sqA);
    a0 = (A + 1) + (A - 1) * cosw0 + sqA;
    a1 = -2 * ((A - 1) + (A + 1) * cosw0);
    a2 = (A + 1) + (A - 1) * cosw0 - sqA;
  } else if (type === "Highshelf") {
    const sqA = 2 * Math.sqrt(A) * alpha;
    b0 = A * ((A + 1) + (A - 1) * cosw0 + sqA);
    b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
    b2 = A * ((A + 1) + (A - 1) * cosw0 - sqA);
    a0 = (A + 1) - (A - 1) * cosw0 + sqA;
    a1 = 2 * ((A - 1) - (A + 1) * cosw0);
    a2 = (A + 1) - (A - 1) * cosw0 - sqA;
  } else {
    b0 = 1 + alpha * A;
    b1 = -2 * cosw0;
    b2 = 1 - alpha * A;
    a0 = 1 + alpha / A;
    a1 = -2 * cosw0;
    a2 = 1 - alpha / A;
  }
  return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function evalBiquadDb([b0, b1, b2, a1, a2], f) {
  const w = (2 * Math.PI * f) / SAMPLE_RATE;
  const cw1 = Math.cos(w);
  const sw1 = Math.sin(w);
  const cw2 = Math.cos(2 * w);
  const sw2 = Math.sin(2 * w);

  const numR = b0 + b1 * cw1 + b2 * cw2;
  const numI = -(b1 * sw1 + b2 * sw2);
  const denR = 1 + a1 * cw1 + a2 * cw2;
  const denI = -(a1 * sw1 + a2 * sw2);

  const magSq = (numR * numR + numI * numI) / Math.max(1e-12, denR * denR + denI * denI);
  return 10 * Math.log10(Math.max(1e-12, magSq));
}

function freqToX(f, width) {
  const minLog = Math.log10(20);
  const maxLog = Math.log10(20000);
  return ((Math.log10(Math.max(20, Math.min(20000, f))) - minLog) / (maxLog - minLog)) * width;
}

function xToFreq(x, width) {
  const minLog = Math.log10(20);
  const maxLog = Math.log10(20000);
  const norm = Math.max(0, Math.min(1, x / width));
  return Math.pow(10, minLog + norm * (maxLog - minLog));
}

function dbToY(db, height) {
  const clamped = Math.max(-20, Math.min(20, db));
  return ((20 - clamped) / 40) * height;
}

function yToDb(y, height) {
  const norm = Math.max(0, Math.min(1, y / height));
  return Math.max(-18, Math.min(18, 20 - norm * 40));
}

function specDbToY(specDb, height) {
  // Map RTA Spectrum (-90 dBFS bottom .. 0 dBFS top) to canvas Y
  const clamped = Math.max(-90, Math.min(0, specDb));
  return ((0 - clamped) / 90) * height;
}

function stepRtaBallistics() {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;

  const rawBins = rawSpectrumByTarget[selectedEqTarget] || rawSpectrumByTarget.master;
  const bands = targetObj.eq_bands || [];
  const coeffList = bands.map((b) =>
    b.enabled === false || targetObj.eq_bypass
      ? [1, 0, 0, 0, 0]
      : biquadCoeffs(b.type, b.freq, b.gain, b.q)
  );

  for (let i = 0; i < NUM_SPEC_BINS; i++) {
    const f = spectrumFreqs[i] || 1000;
    const rawPre = rawBins[i] ?? -95;

    // Calculate net EQ boost/cut at this RTA bin's center frequency
    let eqGainAtF = 0;
    for (let b = 0; b < coeffList.length; b++) {
      eqGainAtF += evalBiquadDb(coeffList[b], f);
    }
    const rawPost = Math.min(6, rawPre + eqGainAtF);

    // Exponential smoothing (fast attack, smooth release)
    const alphaPre = rawPre > smoothPreSpec[i] ? 0.55 : 0.20;
    smoothPreSpec[i] += alphaPre * (rawPre - smoothPreSpec[i]);

    const alphaPost = rawPost > smoothPostSpec[i] ? 0.55 : 0.20;
    smoothPostSpec[i] += alphaPost * (rawPost - smoothPostSpec[i]);

    // Peak hold with 25-frame (~0.8s) hold then smooth decay
    if (smoothPostSpec[i] >= peakHoldSpec[i]) {
      peakHoldSpec[i] = smoothPostSpec[i];
      peakHoldAge[i] = 0;
    } else {
      peakHoldAge[i]++;
      if (peakHoldAge[i] > 20) {
        peakHoldSpec[i] = Math.max(-95, peakHoldSpec[i] - 0.35);
      }
    }
  }
}

function drawRtaSpectrum(w, h) {
  if (rtaMode === "off") return;

  // 1. Pre-EQ Input Spectrum (subtle translucent fill when in "pre_post" mode)
  if (rtaMode === "pre_post") {
    eqCtx.beginPath();
    eqCtx.moveTo(0, h);
    for (let i = 0; i < NUM_SPEC_BINS; i++) {
      const x = freqToX(spectrumFreqs[i], w);
      const y = specDbToY(smoothPreSpec[i], h);
      eqCtx.lineTo(x, y);
    }
    eqCtx.lineTo(w, h);
    eqCtx.closePath();
    eqCtx.fillStyle = "rgba(148, 163, 184, 0.14)";
    eqCtx.fill();

    eqCtx.beginPath();
    for (let i = 0; i < NUM_SPEC_BINS; i++) {
      const x = freqToX(spectrumFreqs[i], w);
      const y = specDbToY(smoothPreSpec[i], h);
      if (i === 0) eqCtx.moveTo(x, y);
      else eqCtx.lineTo(x, y);
    }
    eqCtx.strokeStyle = "rgba(148, 163, 184, 0.38)";
    eqCtx.lineWidth = 1.2;
    eqCtx.stroke();
  }

  // 2. Post-EQ Output Spectrum (vibrant cyan/emerald vertical gradient fill + bar/contour)
  const grad = eqCtx.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0.0, "rgba(34, 211, 238, 0.48)");
  grad.addColorStop(0.55, "rgba(56, 189, 248, 0.24)");
  grad.addColorStop(1.0, "rgba(14, 116, 144, 0.03)");

  eqCtx.beginPath();
  eqCtx.moveTo(0, h);
  for (let i = 0; i < NUM_SPEC_BINS; i++) {
    const x = freqToX(spectrumFreqs[i], w);
    const y = specDbToY(smoothPostSpec[i], h);
    eqCtx.lineTo(x, y);
  }
  eqCtx.lineTo(w, h);
  eqCtx.closePath();
  eqCtx.fillStyle = grad;
  eqCtx.fill();

  // Glowing top contour of Post-EQ RTA
  eqCtx.beginPath();
  for (let i = 0; i < NUM_SPEC_BINS; i++) {
    const x = freqToX(spectrumFreqs[i], w);
    const y = specDbToY(smoothPostSpec[i], h);
    if (i === 0) eqCtx.moveTo(x, y);
    else eqCtx.lineTo(x, y);
  }
  eqCtx.strokeStyle = "rgba(34, 211, 238, 0.85)";
  eqCtx.lineWidth = 1.6;
  eqCtx.stroke();

  // 3. Decaying Peak-Hold Trace
  if (rtaPeakHoldEnabled) {
    eqCtx.beginPath();
    for (let i = 0; i < NUM_SPEC_BINS; i++) {
      const x = freqToX(spectrumFreqs[i], w);
      const y = specDbToY(peakHoldSpec[i], h);
      if (i === 0) eqCtx.moveTo(x, y);
      else eqCtx.lineTo(x, y);
    }
    eqCtx.strokeStyle = "rgba(245, 158, 11, 0.72)";
    eqCtx.lineWidth = 1.3;
    eqCtx.setLineDash([4, 3]);
    eqCtx.stroke();
    eqCtx.setLineDash([]);
  }
}

function drawEqCurve() {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;

  const w = eqCanvas.width;
  const h = eqCanvas.height;
  eqCtx.clearRect(0, 0, w, h);

  // 1. Frequency & dB Grid Lines
  eqCtx.lineWidth = 1;
  const gridFreqs = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  gridFreqs.forEach((gf) => {
    const x = freqToX(gf, w);
    eqCtx.strokeStyle = "#1b2536";
    eqCtx.beginPath();
    eqCtx.moveTo(x, 0);
    eqCtx.lineTo(x, h);
    eqCtx.stroke();

    eqCtx.fillStyle = "#6c7f99";
    eqCtx.font = "11px ui-monospace, monospace";
    const lbl = gf >= 1000 ? `${gf / 1000}k` : `${gf}`;
    eqCtx.fillText(lbl, Math.min(w - 28, x + 4), h - 8);
  });

  [-18, -12, -6, 0, 6, 12, 18].forEach((gdb) => {
    const y = dbToY(gdb, h);
    eqCtx.strokeStyle = gdb === 0 ? "#334663" : "#1b2536";
    eqCtx.beginPath();
    eqCtx.moveTo(0, y);
    eqCtx.lineTo(w, y);
    eqCtx.stroke();

    // Left Axis: EQ Gain (dB)
    eqCtx.fillStyle = "#6c7f99";
    eqCtx.font = "11px ui-monospace, monospace";
    eqCtx.fillText(`${gdb > 0 ? "+" : ""}${gdb} dB`, 6, y - 4);
  });

  // Right Axis: RTA Spectrum Scale (-15 dBFS .. -75 dBFS)
  if (rtaMode !== "off") {
    [-15, -30, -45, -60, -75].forEach((sdb) => {
      const sy = specDbToY(sdb, h);
      eqCtx.fillStyle = "#38bdf899";
      eqCtx.font = "10px ui-monospace, monospace";
      eqCtx.fillText(`${sdb} dBFS`, w - 58, sy - 3);
    });
  }

  // 2. Live RTA Spectrum Analyzer Layer (behind EQ curves)
  drawRtaSpectrum(w, h);

  const bands = targetObj.eq_bands || [];
  const coeffList = bands.map((b) =>
    b.enabled === false || targetObj.eq_bypass
      ? [1, 0, 0, 0, 0]
      : biquadCoeffs(b.type, b.freq, b.gain, b.q)
  );

  // 3. Individual Band Response Curves
  bands.forEach((b, idx) => {
    if (b.enabled === false || Math.abs(b.gain) < 0.05) return;
    const color = BAND_COLORS[idx % BAND_COLORS.length];
    eqCtx.strokeStyle = `${color}66`;
    eqCtx.lineWidth = 1.5;
    eqCtx.beginPath();
    for (let px = 0; px <= w; px += 4) {
      const f = xToFreq(px, w);
      const db = evalBiquadDb(coeffList[idx], f);
      const py = dbToY(db, h);
      if (px === 0) eqCtx.moveTo(px, py);
      else eqCtx.lineTo(px, py);
    }
    eqCtx.stroke();
  });

  // 4. Composite Total Frequency Response Curve
  eqCtx.strokeStyle = targetObj.eq_bypass ? "#6c7f99" : "#f8fafc";
  eqCtx.lineWidth = 2.6;
  eqCtx.beginPath();
  for (let px = 0; px <= w; px += 2) {
    const f = xToFreq(px, w);
    let sumDb = 0;
    for (let i = 0; i < coeffList.length; i++) {
      sumDb += evalBiquadDb(coeffList[i], f);
    }
    const py = dbToY(sumDb, h);
    if (px === 0) eqCtx.moveTo(px, py);
    else eqCtx.lineTo(px, py);
  }
  eqCtx.stroke();

  // 5. Draggable Band Control Handles
  bands.forEach((b, idx) => {
    const nx = freqToX(b.freq, w);
    const ny = dbToY(b.gain, h);
    const color = BAND_COLORS[idx % BAND_COLORS.length];

    eqCtx.beginPath();
    eqCtx.arc(nx, ny, activeDragBandIdx === idx ? 9 : 7, 0, 2 * Math.PI);
    eqCtx.fillStyle = color;
    eqCtx.fill();
    eqCtx.lineWidth = 2;
    eqCtx.strokeStyle = "#090d14";
    eqCtx.stroke();
  });
}

// Smooth Animation Loop for RTA Spectrum Ballistics
function rtaAnimationLoop() {
  if (appState && rtaMode !== "off") {
    stepRtaBallistics();
    drawEqCurve();
  }
  requestAnimationFrame(rtaAnimationLoop);
}
requestAnimationFrame(rtaAnimationLoop);

// Canvas Pointer Dragging for EQ Nodes
eqCanvas.addEventListener("pointerdown", (e) => {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;
  const rect = eqCanvas.getBoundingClientRect();
  const scaleX = eqCanvas.width / rect.width;
  const scaleY = eqCanvas.height / rect.height;
  const mx = (e.clientX - rect.left) * scaleX;
  const my = (e.clientY - rect.top) * scaleY;

  let bestIdx = -1;
  let bestDist = 24;
  targetObj.eq_bands.forEach((b, idx) => {
    const nx = freqToX(b.freq, eqCanvas.width);
    const ny = dbToY(b.gain, eqCanvas.height);
    const dist = Math.hypot(mx - nx, my - ny);
    if (dist < bestDist) {
      bestDist = dist;
      bestIdx = idx;
    }
  });

  if (bestIdx !== -1) {
    activeDragBandIdx = bestIdx;
    eqCanvas.setPointerCapture(e.pointerId);
  }
});

eqCanvas.addEventListener("pointermove", (e) => {
  if (activeDragBandIdx === -1) return;
  const targetObj = getActiveEqObject();
  if (!targetObj) return;
  const rect = eqCanvas.getBoundingClientRect();
  const scaleX = eqCanvas.width / rect.width;
  const scaleY = eqCanvas.height / rect.height;
  const mx = (e.clientX - rect.left) * scaleX;
  const my = (e.clientY - rect.top) * scaleY;

  const band = targetObj.eq_bands[activeDragBandIdx];
  band.freq = Math.round(xToFreq(mx, eqCanvas.width));
  band.gain = Math.round(yToDb(my, eqCanvas.height) * 2) / 2;

  const gSlider = document.getElementById(`eq-gain-${activeDragBandIdx}`);
  const gOut = document.getElementById(`eq-gain-${activeDragBandIdx}-out`);
  const fSlider = document.getElementById(`eq-freq-${activeDragBandIdx}`);
  const fOut = document.getElementById(`eq-freq-${activeDragBandIdx}-out`);
  if (gSlider && gOut) {
    gSlider.value = String(band.gain);
    const gTxt = `${band.gain >= 0 ? "+" : ""}${band.gain.toFixed(1)} dB`;
    gOut.textContent = gTxt;
    syncSliderValue(gSlider, gTxt);
  }
  if (fSlider && fOut) {
    fSlider.value = String(band.freq);
    const fTxt = formatFreq(band.freq);
    fOut.textContent = fTxt;
    syncSliderValue(fSlider, fTxt);
  }

  drawEqCurve();
  pushEqUpdate();
});

eqCanvas.addEventListener("pointerup", () => {
  activeDragBandIdx = -1;
  drawEqCurve();
});

// ---------------------------------------------------------------------------
// Topbar & Toolbar Event Listeners
// ---------------------------------------------------------------------------
document.querySelectorAll(".eq-select-btn").forEach((btn) => {
  btn.addEventListener("click", () => selectEqTarget(btn.dataset.eqTarget));
});

document.getElementById("btn-rta-toggle").addEventListener("click", (e) => {
  const btn = e.currentTarget;
  if (rtaMode === "pre_post") {
    rtaMode = "post";
    btn.textContent = "RTA: Post-EQ";
    btn.setAttribute("aria-pressed", "false");
  } else if (rtaMode === "post") {
    rtaMode = "off";
    btn.textContent = "RTA: Off";
    btn.setAttribute("aria-pressed", "true");
  } else {
    rtaMode = "pre_post";
    btn.textContent = "RTA: Pre+Post";
    btn.setAttribute("aria-pressed", "false");
  }
  drawEqCurve();
});

document.getElementById("btn-rta-peak").addEventListener("click", (e) => {
  rtaPeakHoldEnabled = !rtaPeakHoldEnabled;
  peakHoldSpec.fill(-95);
  e.currentTarget.setAttribute("aria-pressed", String(rtaPeakHoldEnabled));
  e.currentTarget.textContent = rtaPeakHoldEnabled ? "Peak Hold: On" : "Peak Hold: Off";
  drawEqCurve();
});

document.getElementById("btn-eq-bypass").addEventListener("click", () => {
  const targetObj = getActiveEqObject();
  if (!targetObj) return;
  targetObj.eq_bypass = !targetObj.eq_bypass;
  renderEqInspector();
  pushEqUpdate();
});

document.getElementById("btn-eq-flat").addEventListener("click", () => {
  sendWsImmediate({ type: "reset_eq", target: selectedEqTarget });
});

document.getElementById("tab-mixer").addEventListener("click", () => {
  document.getElementById("tab-mixer").classList.add("active");
  document.getElementById("tab-mixer").setAttribute("aria-pressed", "true");
  document.getElementById("tab-master-eq").classList.remove("active");
  document.getElementById("tab-master-eq").setAttribute("aria-pressed", "false");
  document.querySelector(".mixer-rack").style.display = "";
});

document.getElementById("tab-master-eq").addEventListener("click", () => {
  document.getElementById("tab-master-eq").classList.add("active");
  document.getElementById("tab-master-eq").setAttribute("aria-pressed", "true");
  document.getElementById("tab-mixer").classList.remove("active");
  document.getElementById("tab-mixer").setAttribute("aria-pressed", "false");
  selectEqTarget("master");
});

document.querySelectorAll(".test-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    sendWsImmediate({ type: "test_audio", mode: btn.dataset.testMode });
  });
});

function updateTestButtons(mode) {
  document.querySelectorAll(".test-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.testMode === mode && mode !== "stopped");
  });
}

// Presets Dialog (Native <dialog> showModal())
const presetDialog = document.getElementById("preset-dialog");
const presetSelect = document.getElementById("preset-select");
const presetNameInput = document.getElementById("preset-name-input");

document.getElementById("btn-open-preset-dialog").addEventListener("click", () => {
  presetNameInput.value = "";
  presetDialog.showModal();
});

document.getElementById("btn-cancel-preset").addEventListener("click", () => {
  presetDialog.close();
});

document.getElementById("preset-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const name = presetNameInput.value.trim();
  if (name) {
    sendWsImmediate({ type: "save_preset", name });
    presetDialog.close();
  }
});

presetSelect.addEventListener("change", () => {
  if (presetSelect.value) {
    sendWsImmediate({ type: "load_preset", name: presetSelect.value });
  }
});

function updatePresetsDropdown() {
  if (!appState) return;
  const presets = Object.keys(appState.presets || {});
  presetSelect.innerHTML = `<option value="">— Presets (${presets.length}) —</option>`;
  presets.forEach((p) => {
    const opt = document.createElement("option");
    opt.value = p;
    opt.textContent = p;
    presetSelect.appendChild(opt);
  });
}

// ---------------------------------------------------------------------------
// Real-Time WebSocket Connection & 25 FPS VU Meter + RTA Spectrum Updates
// ---------------------------------------------------------------------------
function updateMeters(msg) {
  const capPeak = msg.capture_peak || [];
  const pbPeak = msg.playback_peak || [-100, -100];

  if (msg.spectrum_freqs && msg.spectrum_freqs.length === NUM_SPEC_BINS) {
    spectrumFreqs = msg.spectrum_freqs;
  }
  if (msg.spectrum) {
    rawSpectrumByTarget = msg.spectrum;
  }

  if (appState) {
    appState.channels.forEach((ch, idx) => {
      const lDb = capPeak[idx * 2] ?? -100;
      const rDb = capPeak[idx * 2 + 1] ?? -100;
      const maxDb = Math.max(lDb, rDb);

      const lBar = document.getElementById(`vu-l-${idx}`);
      const rBar = document.getElementById(`vu-r-${idx}`);
      const dbTxt = document.getElementById(`vu-db-${idx}`);
      if (lBar) lBar.style.inlineSize = `${dbToMeterPercent(lDb)}%`;
      if (rBar) rBar.style.inlineSize = `${dbToMeterPercent(rDb)}%`;
      if (dbTxt) dbTxt.textContent = maxDb <= -60 ? "-∞ dBFS" : `${maxDb.toFixed(1)} dBFS`;
    });
  }

  const mL = pbPeak[0] ?? -100;
  const mR = pbPeak[1] ?? -100;
  const mMax = Math.max(mL, mR);
  const mBarL = document.getElementById("vu-master-l");
  const mBarR = document.getElementById("vu-master-r");
  const mDbTxt = document.getElementById("vu-master-db");
  if (mBarL) mBarL.style.inlineSize = `${dbToMeterPercent(mL)}%`;
  if (mBarR) mBarR.style.inlineSize = `${dbToMeterPercent(mR)}%`;
  if (mDbTxt) mDbTxt.textContent = mMax <= -60 ? "-∞ dBFS" : `${mMax.toFixed(1)} dBFS`;

  if (msg.test_mode) {
    updateTestButtons(msg.test_mode);
  }
}

function connectWebSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  const statusEl = document.getElementById("ws-status");
  const statusTxt = document.getElementById("ws-status-text");

  ws.addEventListener("open", () => {
    statusEl.classList.remove("disconnected");
    statusTxt.textContent = "Live DSP + RTA Connected";
  });

  ws.addEventListener("message", (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === "state") {
        appState = msg.state;
        if (msg.spectrum_freqs && msg.spectrum_freqs.length === NUM_SPEC_BINS) {
          spectrumFreqs = msg.spectrum_freqs;
        }
        renderStrips();
        renderEqInspector();
        updatePresetsDropdown();
        if (msg.test_mode) updateTestButtons(msg.test_mode);
      } else if (msg.type === "meters") {
        updateMeters(msg);
      } else if (msg.type === "host_fader_sync") {
        if (appState) {
          const ch = appState.channels.find((c) => c.id === msg.id);
          if (ch) {
            ch.gain_db = msg.gain_db;
            ch.mute = msg.mute;
            const slider = document.getElementById(`fader-${ch.id}`);
            const out = document.getElementById(`fader-${ch.id}-out`);
            if (slider && out) {
              slider.value = String(ch.gain_db);
              const txt = formatDb(ch.gain_db);
              out.textContent = txt;
              syncSliderValue(slider, txt);
            }
            const strip = document.querySelector(`[data-strip-id="${ch.id}"]`);
            if (strip) {
              const muteBtn = strip.querySelector(".btn-mute");
              if (muteBtn) muteBtn.setAttribute("aria-pressed", String(Boolean(ch.mute)));
            }
          }
        }
      } else if (msg.type === "test_status") {
        updateTestButtons(msg.test_mode);
      }
    } catch (err) {
      console.error("WS parse error", err);
    }
  });

  ws.addEventListener("close", () => {
    statusEl.classList.add("disconnected");
    statusTxt.textContent = "Reconnecting…";
    setTimeout(connectWebSocket, 1500);
  });
}

connectWebSocket();
