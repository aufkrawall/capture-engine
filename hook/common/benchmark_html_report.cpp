#include "benchmark_html_report.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "hook_common.h"

namespace {

std::string SanitizeFilename(const std::string& name) {
    std::string clean;
    clean.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            clean.push_back(c);
        } else if (c == ' ' || c == '.') {
            clean.push_back('_');
        }
    }
    return clean.empty() ? "Benchmark" : clean;
}

std::filesystem::path ResolveBenchmarksDirectory(const std::string& configuredOutputDir) {
    std::error_code ec;
    if (!configuredOutputDir.empty()) {
        std::filesystem::path dir(configuredOutputDir);
        if (std::filesystem::create_directories(dir, ec) || std::filesystem::exists(dir, ec)) {
            return dir;
        }
    }

    // Default: "benchmarks" subdirectory next to the executable
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0) {
        std::filesystem::path baseDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path benchDir = baseDir / "benchmarks";
        std::filesystem::create_directories(benchDir, ec);
        if (std::filesystem::exists(benchDir, ec)) {
            return benchDir;
        }
    }

    std::filesystem::path fallback("benchmarks");
    std::filesystem::create_directories(fallback, ec);
    return fallback;
}

}  // namespace

std::string SaveBenchmarkHtmlReport(const BenchmarkResults& results, const std::string& configuredOutputDir) {
    std::filesystem::path outDir = ResolveBenchmarksDirectory(configuredOutputDir);

    // Create file name: <ProfileName>_YYYY-MM-DD_HH-MM-SS.html
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf = {};
    localtime_s(&tmBuf, &nowTime);

    std::ostringstream timeStr;
    timeStr << std::put_time(&tmBuf, "%Y-%m-%d_%H-%M-%S");

    std::string safeName = SanitizeFilename(results.profileName.empty() ? results.executableName : results.profileName);
    std::string fileName = safeName + "_" + timeStr.str() + ".html";
    std::filesystem::path fullPath = outDir / fileName;

    std::ofstream out(fullPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        HookLog("[Benchmark] Failed to create HTML report file at %ls", fullPath.c_str());
        return "";
    }

    // Serialize data as JSON columns
    std::ostringstream jsonTimes, jsonPresFps, jsonDispFps, jsonPresMs, jsonDispMs;
    std::ostringstream jsonCpu, jsonCpuMax, jsonGpu, jsonRam, jsonVram;
    std::ostringstream jsonCpuTemp, jsonGpuTemp, jsonCpuPower, jsonGpuPower;
    std::ostringstream jsonGpuFan, jsonCpuClock, jsonGpuClock, jsonGpuMemClock, jsonGpuVoltage;

    for (size_t i = 0; i < results.records.size(); ++i) {
        const auto& r = results.records[i];
        const char* sep = (i == 0) ? "" : ",";

        jsonTimes << sep << std::fixed << std::setprecision(3) << r.timeSeconds;
        jsonPresFps << sep << std::fixed << std::setprecision(1) << r.presentationFps;
        jsonDispFps << sep << std::fixed << std::setprecision(1) << r.displayFps;
        jsonPresMs << sep << std::fixed << std::setprecision(2) << r.presentationFrameTimeMs;
        jsonDispMs << sep << std::fixed << std::setprecision(2) << r.displayFrameTimeMs;

        jsonCpu << sep << std::fixed << std::setprecision(1) << r.cpuUsage;
        jsonCpuMax << sep << std::fixed << std::setprecision(1) << r.cpuMaxCoreUsage;
        jsonGpu << sep << std::fixed << std::setprecision(1) << r.gpuUsage;
        jsonRam << sep << std::fixed << std::setprecision(2) << r.ramUsedGb;
        jsonVram << sep << std::fixed << std::setprecision(2) << r.vramUsedGb;

        jsonCpuTemp << sep << std::fixed << std::setprecision(1) << r.cpuTempC;
        jsonGpuTemp << sep << std::fixed << std::setprecision(1) << r.gpuTempC;
        jsonCpuPower << sep << std::fixed << std::setprecision(1) << r.cpuPowerW;
        jsonGpuPower << sep << std::fixed << std::setprecision(1) << r.gpuPowerW;
        jsonGpuFan << sep << std::fixed << std::setprecision(0) << r.gpuFanRpm;
        jsonCpuClock << sep << std::fixed << std::setprecision(0) << r.cpuClockMhz;
        jsonGpuClock << sep << std::fixed << std::setprecision(0) << r.gpuClockMhz;
        jsonGpuMemClock << sep << std::fixed << std::setprecision(0) << r.gpuMemClockMhz;
        jsonGpuVoltage << sep << std::fixed << std::setprecision(3) << r.gpuVoltageV;
    }

    out << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CaptureEngine Benchmark - )HTML" << safeName << R"HTML(</title>
<style>
:root {
  --bg: #0d1117; --card-bg: #161b22; --border: #30363d;
  --text: #e6edf3; --text-muted: #8b949e; --accent-blue: #58a6ff;
  --accent-green: #3fb950; --accent-orange: #d29922; --accent-red: #f85149;
  --accent-purple: #bc8cff; --accent-cyan: #39c5bb;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  background-color: var(--bg); color: var(--text); padding: 24px;
}
.container { max-width: 1280px; margin: 0 auto; display: flex; flex-direction: column; gap: 20px; }
.header {
  background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px;
  padding: 20px 24px; display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 16px;
}
.header-title h1 { font-size: 1.5rem; font-weight: 700; color: #fff; }
.header-title p { color: var(--text-muted); font-size: 0.9rem; margin-top: 4px; }
.meta-badges { display: flex; flex-wrap: wrap; gap: 8px; }
.badge {
  background: #21262d; border: 1px solid var(--border); border-radius: 6px;
  padding: 6px 12px; font-size: 0.82rem; color: var(--text);
}
.mode-bar {
  display: flex; gap: 10px; align-items: center;
}
.mode-btn {
  background: #21262d; border: 1px solid var(--border); color: var(--text-muted);
  padding: 8px 16px; border-radius: 6px; font-size: 0.9rem; font-weight: 600;
  cursor: pointer; transition: all 0.15s ease;
}
.mode-btn.active {
  background: #1f6feb; border-color: #388bfd; color: #fff;
}
.kpi-grid {
  display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 16px;
}
.kpi-card {
  background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px;
  padding: 16px 20px; text-align: center;
}
.kpi-label { font-size: 0.85rem; color: var(--text-muted); text-transform: uppercase; font-weight: 600; }
.kpi-value { font-size: 2rem; font-weight: 700; margin: 8px 0 4px 0; }
.kpi-sub { font-size: 0.8rem; color: var(--text-muted); }
.val-avg { color: var(--accent-green); }
.val-1pct { color: var(--accent-orange); }
.val-01pct { color: var(--accent-red); }
.val-max { color: var(--accent-blue); }
.val-min { color: var(--accent-purple); }
.chart-card {
  background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px;
  padding: 20px; position: relative;
}
.chart-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
.chart-title { font-size: 1.1rem; font-weight: 600; color: #fff; }
.chart-canvas-wrap { width: 100%; height: 360px; position: relative; }
canvas { width: 100%; height: 100%; display: block; }
.hud-tooltip {
  position: absolute; top: 12px; right: 12px; background: rgba(13, 17, 23, 0.92);
  border: 1px solid var(--border); border-radius: 6px; padding: 10px 14px;
  font-size: 0.82rem; pointer-events: none; z-index: 10; display: none;
  box-shadow: 0 4px 12px rgba(0,0,0,0.5); min-width: 240px;
}
.hud-title { font-weight: 700; color: var(--accent-blue); margin-bottom: 6px; border-bottom: 1px solid var(--border); padding-bottom: 4px; }
.hud-row { display: flex; justify-content: space-between; margin-bottom: 3px; }
.hud-k { color: var(--text-muted); }
.hud-v { font-weight: 600; color: var(--text); }
.table-card {
  background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px;
  padding: 20px;
}
table { width: 100%; border-collapse: collapse; font-size: 0.9rem; }
th, td { padding: 10px 14px; text-align: left; border-bottom: 1px solid var(--border); }
th { color: var(--text-muted); font-size: 0.8rem; text-transform: uppercase; }
tr:hover { background: rgba(255,255,255,0.02); }
.footer { text-align: center; color: var(--text-muted); font-size: 0.8rem; padding: 12px; }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="header-title">
      <h1>)HTML" << safeName << R"HTML( Benchmark Report</h1>
      <p>Recorded on )HTML" << results.timestampStr << R"HTML( • )HTML"
      << std::fixed << std::setprecision(1) << results.durationSeconds << R"HTML( s • )HTML"
      << results.totalFrames << R"HTML( frames</p>
    </div>
    <div class="meta-badges">
      <div class="badge">CPU: )HTML" << (results.cpuName.empty() ? "System CPU" : results.cpuName) << R"HTML(</div>
      <div class="badge">GPU: )HTML" << (results.gpuName.empty() ? "System GPU" : results.gpuName) << R"HTML(</div>
      <div class="badge">RAM: )HTML" << std::fixed << std::setprecision(1) << results.ramTotalGb << R"HTML( GB</div>
      <div class="badge">VRAM: )HTML" << std::fixed << std::setprecision(1) << results.vramTotalGb << R"HTML( GB</div>
    </div>
  </div>

  <div class="mode-bar">
    <button class="mode-btn active" id="btn-pres" onclick="setMode('pres')">Classic Frame Start (Presentation)</button>
    <button class="mode-btn" id="btn-disp" onclick="setMode('disp')">msBetweenDisplayChange (Display Cadence)</button>
  </div>

  <div class="kpi-grid">
    <div class="kpi-card">
      <div class="kpi-label">Average FPS</div>
      <div class="kpi-value val-avg" id="kpi-avg">0.0</div>
      <div class="kpi-sub" id="kpi-avg-ms">0.0 ms</div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">1% Low FPS</div>
      <div class="kpi-value val-1pct" id="kpi-1pct">0.0</div>
      <div class="kpi-sub">99th percentile slowest</div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">0.1% Low FPS</div>
      <div class="kpi-value val-01pct" id="kpi-01pct">0.0</div>
      <div class="kpi-sub">99.9th percentile slowest</div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">Max FPS</div>
      <div class="kpi-value val-max" id="kpi-max">0.0</div>
      <div class="kpi-sub">Fastest frame</div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">Min FPS</div>
      <div class="kpi-value val-min" id="kpi-min">0.0</div>
      <div class="kpi-sub">Slowest frame</div>
    </div>
  </div>

  <div class="chart-card">
    <div class="chart-header">
      <div class="chart-title">Framerate Over Time</div>
      <div style="font-size: 0.85rem; color: var(--text-muted);">Hover cursor over graph to inspect live telemetry</div>
    </div>
    <div class="chart-canvas-wrap">
      <canvas id="chartCanvas"></canvas>
      <div class="hud-tooltip" id="hudTooltip"></div>
    </div>
  </div>

  <div class="table-card">
    <h2 style="font-size: 1.1rem; margin-bottom: 14px; font-weight: 600;">Hardware & Telemetry Sensors</h2>
    <table>
      <thead>
        <tr><th>Sensor Metric</th><th>Average</th><th>Maximum</th><th>Minimum</th><th>Unit</th></tr>
      </thead>
      <tbody>)HTML";

    auto printRow = [&](const char* name, const BenchmarkSensorSummary& s, const char* unit, int precision) {
        if (!s.valid) return;
        out << "<tr><td>" << name << "</td><td>"
            << std::fixed << std::setprecision(precision) << s.avg << "</td><td>"
            << std::fixed << std::setprecision(precision) << s.max << "</td><td>"
            << std::fixed << std::setprecision(precision) << s.min << "</td><td>"
            << unit << "</td></tr>";
    };

    printRow("CPU Total Load", results.cpuUsage, "%", 1);
    printRow("CPU Max Single Core Load", results.cpuMaxCoreUsage, "%", 1);
    printRow("CPU Temperature", results.cpuTemp, "C", 1);
    printRow("CPU Package Power", results.cpuPower, "W", 1);
    printRow("CPU Core Clock", results.cpuClock, "MHz", 0);
    printRow("GPU Usage", results.gpuUsage, "%", 1);
    printRow("GPU Temperature", results.gpuTemp, "C", 1);
    printRow("GPU Package Power", results.gpuPower, "W", 1);
    printRow("GPU Fan Speed", results.gpuFan, "RPM", 0);
    printRow("GPU Core Clock", results.gpuClock, "MHz", 0);
    printRow("GPU Memory Clock", results.gpuMemClock, "MHz", 0);
    printRow("GPU Core Voltage", results.gpuVoltage, "V", 3);
    printRow("System RAM Usage", results.ramUsedGb, "GB", 2);
    printRow("Dedicated VRAM Usage", results.vramUsedGb, "GB", 2);

    out << R"HTML(
      </tbody>
    </table>
  </div>
  <div class="footer">CaptureEngine • High Performance Capture & Diagnostics Suite</div>
</div>

<script>
const DATA = {
  times: [)HTML" << jsonTimes.str() << R"HTML(],
  presFps: [)HTML" << jsonPresFps.str() << R"HTML(],
  dispFps: [)HTML" << jsonDispFps.str() << R"HTML(],
  presMs: [)HTML" << jsonPresMs.str() << R"HTML(],
  dispMs: [)HTML" << jsonDispMs.str() << R"HTML(],
  cpu: [)HTML" << jsonCpu.str() << R"HTML(],
  cpuMax: [)HTML" << jsonCpuMax.str() << R"HTML(],
  gpu: [)HTML" << jsonGpu.str() << R"HTML(],
  ram: [)HTML" << jsonRam.str() << R"HTML(],
  vram: [)HTML" << jsonVram.str() << R"HTML(],
  cpuTemp: [)HTML" << jsonCpuTemp.str() << R"HTML(],
  gpuTemp: [)HTML" << jsonGpuTemp.str() << R"HTML(],
  cpuPower: [)HTML" << jsonCpuPower.str() << R"HTML(],
  gpuPower: [)HTML" << jsonGpuPower.str() << R"HTML(],
  gpuFan: [)HTML" << jsonGpuFan.str() << R"HTML(],
  cpuClock: [)HTML" << jsonCpuClock.str() << R"HTML(],
  gpuClock: [)HTML" << jsonGpuClock.str() << R"HTML(],
  gpuMemClock: [)HTML" << jsonGpuMemClock.str() << R"HTML(],
  gpuVoltage: [)HTML" << jsonGpuVoltage.str() << R"HTML(],
  stats: {
    pres: {
      avg: )HTML" << results.presentationStats.avgFps << R"HTML(,
      avgMs: )HTML" << results.presentationStats.avgFrameTimeMs << R"HTML(,
      onePct: )HTML" << results.presentationStats.onePercentLowFps << R"HTML(,
      zeroOnePct: )HTML" << results.presentationStats.zeroPointOnePercentLowFps << R"HTML(,
      max: )HTML" << results.presentationStats.maxFps << R"HTML(,
      min: )HTML" << results.presentationStats.minFps << R"HTML(
    },
    disp: {
      avg: )HTML" << results.displayStats.avgFps << R"HTML(,
      avgMs: )HTML" << results.displayStats.avgFrameTimeMs << R"HTML(,
      onePct: )HTML" << results.displayStats.onePercentLowFps << R"HTML(,
      zeroOnePct: )HTML" << results.displayStats.zeroPointOnePercentLowFps << R"HTML(,
      max: )HTML" << results.displayStats.maxFps << R"HTML(,
      min: )HTML" << results.displayStats.minFps << R"HTML(
    }
  }
};

let currentMode = 'pres';
function setMode(mode) {
  currentMode = mode;
  document.getElementById('btn-pres').className = mode === 'pres' ? 'mode-btn active' : 'mode-btn';
  document.getElementById('btn-disp').className = mode === 'disp' ? 'mode-btn active' : 'mode-btn';
  const st = DATA.stats[mode];
  document.getElementById('kpi-avg').innerText = st.avg.toFixed(1);
  document.getElementById('kpi-avg-ms').innerText = st.avgMs.toFixed(2) + ' ms';
  document.getElementById('kpi-1pct').innerText = st.onePct.toFixed(1);
  document.getElementById('kpi-01pct').innerText = st.zeroOnePct.toFixed(1);
  document.getElementById('kpi-max').innerText = st.max.toFixed(1);
  document.getElementById('kpi-min').innerText = st.min.toFixed(1);
  renderChart();
}

const canvas = document.getElementById('chartCanvas');
const ctx = canvas.getContext('2d');
const tooltip = document.getElementById('hudTooltip');
let hoverIndex = -1;

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.parentElement.getBoundingClientRect();
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  renderChart();
}
window.addEventListener('resize', resizeCanvas);

function renderChart() {
  const rect = canvas.parentElement.getBoundingClientRect();
  const w = rect.width;
  const h = rect.height;
  ctx.clearRect(0, 0, w, h);

  const fps = currentMode === 'pres' ? DATA.presFps : DATA.dispFps;
  const len = fps.length;
  if (len < 2) return;

  const padLeft = 45, padRight = 20, padTop = 20, padBottom = 30;
  const plotW = w - padLeft - padRight;
  const plotH = h - padTop - padBottom;

  let maxFpsVal = Math.max(...fps, 60);
  maxFpsVal = Math.ceil(maxFpsVal / 30) * 30;
  const minFpsVal = 0;

  // Grid lines
  ctx.strokeStyle = '#21262d';
  ctx.lineWidth = 1;
  ctx.fillStyle = '#8b949e';
  ctx.font = '11px sans-serif';
  ctx.textAlign = 'right';

  const steps = 4;
  for (let i = 0; i <= steps; i++) {
    const val = minFpsVal + ((maxFpsVal - minFpsVal) * (i / steps));
    const y = padTop + plotH - (plotH * (i / steps));
    ctx.beginPath();
    ctx.moveTo(padLeft, y);
    ctx.lineTo(w - padRight, y);
    ctx.stroke();
    ctx.fillText(Math.round(val), padLeft - 8, y + 4);
  }

  // Time ticks
  ctx.textAlign = 'center';
  const totalTime = DATA.times[len - 1] || 1;
  for (let t = 0; t <= totalTime; t += Math.max(5, Math.round(totalTime / 6))) {
    const x = padLeft + (plotW * (t / totalTime));
    ctx.fillText(t + 's', x, h - 8);
  }

  // Plot FPS Curve
  const points = [];
  for (let i = 0; i < len; i++) {
    const t = DATA.times[i];
    const x = padLeft + (plotW * (t / totalTime));
    const y = padTop + plotH - (plotH * (Math.min(fps[i], maxFpsVal) / maxFpsVal));
    points.push({ x, y });
  }

  // Area fill
  const grad = ctx.createLinearGradient(0, padTop, 0, padTop + plotH);
  grad.addColorStop(0, 'rgba(57, 197, 187, 0.35)');
  grad.addColorStop(1, 'rgba(57, 197, 187, 0.02)');
  ctx.fillStyle = grad;
  ctx.beginPath();
  ctx.moveTo(points[0].x, padTop + plotH);
  for (const pt of points) ctx.lineTo(pt.x, pt.y);
  ctx.lineTo(points[points.length - 1].x, padTop + plotH);
  ctx.closePath();
  ctx.fill();

  // Line stroke
  ctx.strokeStyle = '#39c5bb';
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 1; i < points.length; i++) ctx.lineTo(points[i].x, points[i].y);
  ctx.stroke();

  // Hover crosshair & point
  if (hoverIndex >= 0 && hoverIndex < points.length) {
    const pt = points[hoverIndex];
    ctx.strokeStyle = '#e6edf3';
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(pt.x, padTop);
    ctx.lineTo(pt.x, padTop + plotH);
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.fillStyle = '#fff';
    ctx.beginPath();
    ctx.arc(pt.x, pt.y, 5, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = '#388bfd';
    ctx.lineWidth = 2;
    ctx.stroke();
  }
}

canvas.addEventListener('mousemove', (e) => {
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left;
  const padLeft = 45, padRight = 20;
  const plotW = rect.width - padLeft - padRight;
  const totalTime = DATA.times[DATA.times.length - 1] || 1;

  if (mx < padLeft || mx > rect.width - padRight) {
    tooltip.style.display = 'none';
    hoverIndex = -1;
    renderChart();
    return;
  }

  const normX = (mx - padLeft) / plotW;
  const targetTime = normX * totalTime;
  let idx = 0;
  let minDiff = Math.abs(DATA.times[0] - targetTime);
  for (let i = 1; i < DATA.times.length; i++) {
    const diff = Math.abs(DATA.times[i] - targetTime);
    if (diff < minDiff) { minDiff = diff; idx = i; }
  }

  hoverIndex = idx;
  renderChart();

  const fpsVal = currentMode === 'pres' ? DATA.presFps[idx] : DATA.dispFps[idx];
  const msVal = currentMode === 'pres' ? DATA.presMs[idx] : DATA.dispMs[idx];

  let rows = `
    <div class="hud-title">${DATA.times[idx].toFixed(2)}s (Frame #${idx + 1})</div>
    <div class="hud-row"><span class="hud-k">Framerate:</span><span class="hud-v" style="color:#39c5bb;">${fpsVal.toFixed(1)} FPS (${msVal.toFixed(2)} ms)</span></div>
    <div class="hud-row"><span class="hud-k">CPU Load:</span><span class="hud-v">${DATA.cpu[idx].toFixed(1)}% (Max Core: ${DATA.cpuMax[idx].toFixed(1)}%)</span></div>
  `;
  if (DATA.cpuTemp[idx] > 0) rows += `<div class="hud-row"><span class="hud-k">CPU Temp / Power:</span><span class="hud-v">${DATA.cpuTemp[idx].toFixed(0)}°C / ${DATA.cpuPower[idx].toFixed(0)}W</span></div>`;
  if (DATA.cpuClock[idx] > 0) rows += `<div class="hud-row"><span class="hud-k">CPU Clock:</span><span class="hud-v">${DATA.cpuClock[idx].toFixed(0)} MHz</span></div>`;
  rows += `<div class="hud-row"><span class="hud-k">GPU Usage:</span><span class="hud-v">${DATA.gpu[idx].toFixed(1)}%</span></div>`;
  if (DATA.gpuTemp[idx] > 0) rows += `<div class="hud-row"><span class="hud-k">GPU Temp / Power:</span><span class="hud-v">${DATA.gpuTemp[idx].toFixed(0)}°C / ${DATA.gpuPower[idx].toFixed(0)}W</span></div>`;
  if (DATA.gpuClock[idx] > 0) rows += `<div class="hud-row"><span class="hud-k">GPU Clock / Voltage:</span><span class="hud-v">${DATA.gpuClock[idx].toFixed(0)} MHz / ${DATA.gpuVoltage[idx].toFixed(2)}V</span></div>`;
  if (DATA.gpuFan[idx] > 0) rows += `<div class="hud-row"><span class="hud-k">GPU Fan:</span><span class="hud-v">${DATA.gpuFan[idx].toFixed(0)} RPM</span></div>`;
  rows += `<div class="hud-row"><span class="hud-k">VRAM / RAM:</span><span class="hud-v">${DATA.vram[idx].toFixed(1)} GB / ${DATA.ram[idx].toFixed(1)} GB</span></div>`;

  tooltip.innerHTML = rows;
  tooltip.style.display = 'block';
});

canvas.addEventListener('mouseleave', () => {
  tooltip.style.display = 'none';
  hoverIndex = -1;
  renderChart();
});

setMode('pres');
resizeCanvas();
</script>
</body>
</html>
)HTML";

    out.close();
    HookLogImportant("[Benchmark] HTML report written to: %ls", fullPath.c_str());

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 1) {
        std::string resultStr(utf8Len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, resultStr.data(), utf8Len, nullptr, nullptr);
        return resultStr;
    }
    return fullPath.string();
}
