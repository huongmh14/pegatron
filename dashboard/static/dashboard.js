"use strict";

const $ = id => document.getElementById(id);
const trays = ["S", "M", "L"];
const modes = { idle: "Đang tạm dừng", sorting: "Đang phân loại", maintenance: "Bảo trì", fault: "Đã khóa do lỗi" };
const reasons = {
  OFFLINE: "Chưa có kết nối thiết bị đang hoạt động.",
  MONITOR_ONLY: "Máy thật đang ở chế độ chỉ giám sát.",
  ESTOP_UNKNOWN: "Chưa xác nhận được trạng thái dừng khẩn tại máy.",
  ESTOP: "Dừng khẩn đang tác động.",
  ESTOP_ACTIVE: "Dừng khẩn đang tác động hoặc cần xác nhận lại tại máy.",
  STOP_ACTIVE: "Nút STOP đang nhấn hoặc mạch STOP bị hở.",
  RELAY_UNCONFIGURED: "Chưa xác nhận mức kích relay; không thể chạy.",
  FAULT_LOCKED: "Xử lý và xóa lỗi tại máy.",
  LOCAL_ARM_REQUIRED: "Cần cấp quyền điều khiển tại máy.",
  IDLE_AND_HOME_REQUIRED: "Cần tạm dừng và xác nhận cả hai cánh ở HOME.",
  MAINTENANCE_REQUIRED: "Cần chuyển sang bảo trì tại máy.",
  HOME_REQUIRED: "Cánh gạt chưa về HOME.",
  HOME_TIMEOUT_1: "Cánh gạt trạm 1 không về HOME đúng hạn.",
  HOME_TIMEOUT_2: "Cánh gạt trạm 2 không về HOME đúng hạn.",
  TOF_UNAVAILABLE_1: "Trạm 1 chưa đọc được cảm biến ToF.",
  TOF_UNAVAILABLE_2: "Trạm 2 chưa đọc được cảm biến ToF.",
  COMMAND_PENDING: "Đang chờ lệnh trước hoàn tất.",
  ALREADY_PAUSED: "Phân loại đang tạm dừng.",
  WAITING_ACK: "Đang chờ Pico.",
  ACK_TIMEOUT: "Đã quá thời gian chờ Pico. Hãy kiểm tra trạng thái thực tế.",
  BACKEND_RESTART: "Máy chủ đã khởi động lại trước khi nhận xác nhận.",
  MQTT_SEND_UNCONFIRMED: "Chưa xác nhận đã gửi được qua MQTT.",
  LOCAL_OVERRIDE: "Điều kiện điều khiển tại máy đã thay đổi.",
  LINK_LOST: "Kết nối thiết bị đã mất.",
  EXPIRED: "Lệnh đã hết hạn.",
  PAUSED: "Thao tác bị hủy do lệnh tạm dừng.",
  CALIBRATED: "Đã cập nhật mốc mặt băng.",
  TOO_FEW_SAMPLES: "Thiếu mẫu hợp lệ.",
  UNSTABLE_SURFACE: "Mặt băng trống chưa ổn định.",
  REFERENCE_OUT_OF_RANGE: "Mốc mới vượt giới hạn hiệu chuẩn.",
  RUN_OPEN: "Kết thúc buổi đang mở trước khi bắt đầu buổi mới.",
  RUN_NOT_OPEN: "Buổi chạy đã kết thúc.",
  NETWORK_ERROR: "Mất kết nối với máy chủ dashboard.",
  OK: "Pico đã xác nhận."
};
const actionNames = {
  ENABLE_SORTING: "Bật phân loại", PAUSE_SORTING: "Tạm dừng phân loại",
  TEST_PUSHER: "Thử một nhịp gạt", CALIBRATE_TOF: "Hiệu chuẩn ToF"
};
const results = {pending: "Đang chờ Pico", executed: "Đã thực hiện", rejected: "Bị từ chối", unknown: "Chưa xác nhận"};
const kinds = {fault_raised: "Lỗi", fault_cleared: "Xóa lỗi", mode_changed: "Chế độ", calibrated: "Hiệu chuẩn"};
const numbers = new Intl.NumberFormat("vi-VN", {maximumFractionDigits: 2});
const clock = new Intl.DateTimeFormat("vi-VN", {hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false});
const dateTime = new Intl.DateTimeFormat("vi-VN", {dateStyle: "short", timeStyle: "medium", hour12: false});
const describe = code => reasons[code] || code;
const value = (n, unit = "") => n == null ? "—" : numbers.format(n) + unit;
let state = null;
let requestOffline = false;
let controlStatus = null;
let selectedRun = "";
let runsSignature = "";
let commandVersion = 0;
let runSaving = false;
let renderedLive = null;

function write(id, text) {
  const node = $(id);
  if (node.textContent !== text) node.textContent = text;
}

for (const i of [1, 2]) {
  const targetSize = i === 1 ? "L" : "S";
  const targetClass = i === 1 ? "l" : "s";
  $("stations").insertAdjacentHTML("beforeend", `
    <article class="station">
      <div class="station-head"><h3>Trạm ${i} · <span class="${targetClass}">Gạt ${targetSize}</span></h3><span class="badge neutral" id="home-${i}">HOME chưa rõ</span></div>
      <dl class="station-values">
        <div><dt id="measure-label-${i}">Chiều cao</dt><dd class="metric-big" id="height-${i}">—</dd><dd class="sample" id="valid-${i}"></dd></div>
        <div><dt>Khoảng cách ToF</dt><dd class="metric-sub" id="distance-${i}">—</dd></div>
        <div><dt>Nhịp gạt</dt><dd id="push-${i}">—</dd></div>
        <div><dt>Mốc mặt băng</dt><dd id="reference-${i}">—</dd></div>
      </dl>
      <div class="meter" aria-hidden="true"><div class="meter-fill meter-${targetClass}" id="meter-${i}"></div></div>
    </article>`);
}
for (const [tray, label] of [["S", "Thấp"], ["M", "Trung bình"], ["L", "Cao"]]) {
  $("counts").insertAdjacentHTML("beforeend", `
    <div class="tray-card tray-card-${tray}">
      <dt><span class="tray tray-${tray}">${tray}</span>${label}</dt>
      <dd id="tray-${tray}">—</dd>
      <div class="share-bar" aria-hidden="true"><div class="share-fill share-${tray}" id="share-${tray}"></div></div>
    </div>`);
}

const controls = [
  ["Phân loại tự động", [["ENABLE_SORTING", null], ["PAUSE_SORTING", null]]],
  ["Thử cơ cấu gạt", [["TEST_PUSHER", 1], ["TEST_PUSHER", 2]]],
  ["Hiệu chuẩn mốc mặt băng", [["CALIBRATE_TOF", 1], ["CALIBRATE_TOF", 2]]]
].flatMap(([title, items]) => {
  const group = document.createElement("div");
  group.className = "control-group";
  group.innerHTML = `<h3>${title}</h3>`;
  $("controls").append(group);
  return items.map(([action, station]) => {
    const row = document.createElement("div");
    row.className = "control-item";
    const button = document.createElement("button");
    const help = document.createElement("small");
    help.id = `why-${action}-${station || 0}`;
    help.textContent = reasons.OFFLINE;
    button.type = "button";
    button.disabled = true;
    button.className = action === "ENABLE_SORTING" ? "" : "secondary";
    button.textContent = station ? `${actionNames[action]} · Trạm ${station}` : actionNames[action];
    button.setAttribute("aria-describedby", help.id);
    button.addEventListener("click", () => sendCommand(action, station));
    row.append(button, help);
    group.append(row);
    return {action, station, button, help};
  });
});

async function api(url, body) {
  const options = {cache: "no-store", signal: AbortSignal.timeout(1800)};
  if (body !== undefined) Object.assign(options, {
    method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)
  });
  let response;
  try {
    response = await fetch(url, options);
  } catch {
    throw new Error("NETWORK_ERROR");
  }
  const data = await response.json();
  if (!response.ok) throw new Error(typeof data.detail === "string" ? data.detail : "Dữ liệu gửi chưa hợp lệ.");
  return data;
}

function isLive() {
  return !requestOffline && state?.online && Date.now() - state.received_at * 1000 < 3000;
}

function connection() {
  const live = Boolean(isLive());
  const connected = $("connection");
  connected.className = "badge " + (live ? "good" : "warning");
  write("connection", live ? "Đang kết nối" : "Mất kết nối");
  $("stale-warning").hidden = live || !state?.snapshot;
  write("rate", live ? value(state?.confirmed_per_second, " sp/s") : "—");
  $("start-run").disabled = !live || Boolean(state?.active_run) || runSaving;
  for (const item of controls) {
    const rule = state?.controls[item.action];
    const reason = live ? (item.station ? rule?.[item.station] : rule) : "OFFLINE";
    const busy = controlStatus && (item.action !== "PAUSE_SORTING" || controlStatus === "PAUSE_SORTING");
    item.button.disabled = reason !== null || Boolean(busy);
    item.help.textContent = reason ? describe(reason) : busy ? reasons.COMMAND_PENDING : "Có thể gửi lệnh.";
  }
  renderedLive = live;
}

function renderStatus() {
  const sample = state?.snapshot;
  const source = $("source");
  source.hidden = !state;
  source.className = "badge " + (state?.simulated ? "warning" : "neutral");
  write("source", state?.simulated ? "GIẢ LẬP" : state?.device_id || "");
  const seqText = sample?.seq != null ? ` · Gói #${sample.seq}` : "";
  write("updated", state?.received_at ? "Cập nhật " + dateTime.format(state.received_at * 1000) + seqText : "Đang chờ dữ liệu thiết bị");
  write("mode", sample ? modes[sample.mode] : "—");
  const modeNode = $("mode");
  modeNode.className = sample ? (sample.mode === "sorting" ? "mode-sorting" : sample.mode === "fault" ? "mode-fault" : "mode-idle") : "";
  write("estop", sample?.estop == null ? "Chưa có phản hồi" : sample.estop ? "Đang tác động" : "Sẵn sàng");
  write("faults", sample ? sample.faults.map(describe).join(" · ") || "Không có lỗi" : "—");
  $("faults").className = sample?.faults?.length ? "fault-active" : "";
  for (const i of [1, 2]) {
    const station = sample?.[`station_${i}`];
    const home = station?.home;
    $(`home-${i}`).className = "badge " + (home == null ? "neutral" : home ? "good" : "warning");
    write(`home-${i}`, home == null ? "HOME chưa rõ" : home ? "Đã về HOME" : "Ngoài HOME");
    const rawOnly = station?.height_mm == null && station?.distance_mm != null;
    write(`measure-label-${i}`, rawOnly ? "Khoảng cách ToF" : "Chiều cao");
    const displayHeight = station?.height_mm == null ? null : Math.max(0, station.height_mm);
    const metricVal = rawOnly ? station.distance_mm : displayHeight;
    write(`height-${i}`, value(metricVal, " mm"));
    write(`distance-${i}`, value(station?.distance_mm, " mm"));
    write(`valid-${i}`, station ? station.sample_valid ? "Mẫu hợp lệ" : "Mẫu không hợp lệ" : "");
    write(`push-${i}`, value(station?.push_count));
    write(`reference-${i}`, value(station?.reference_mm, " mm"));
    const pct = metricVal == null ? 0 : Math.max(0, Math.min(100, (metricVal / 100) * 100));
    $(`meter-${i}`).style.width = `${pct}%`;
    const active = Boolean(station?.sample_valid) && (rawOnly ? station.distance_mm < 77 : (station.height_mm ?? 0) > 5);
    $(`belt-node-${i}`)?.classList.toggle("active", active);
  }
  const run = state?.active_run;
  write("count-scope", run ? `Số đếm xác nhận từ đầu buổi chạy · ${run.name}` : "Số đếm cộng dồn từ phiên thiết bị.");
  const countMap = run ? state.run_counts : sample?.trays;
  const totalCount = trays.reduce((sum, tray) => sum + (Number(countMap?.[tray]) || 0), 0);
  for (const tray of trays) {
    const n = countMap?.[tray];
    const missing = Boolean(sample) && n == null;
    $(`tray-${tray}`).classList.toggle("missing", missing);
    write(`tray-${tray}`, missing ? "Chưa xác nhận" : value(n));
    const sharePct = totalCount > 0 && n != null ? Math.round((Number(n) / totalCount) * 100) : 0;
    $(`share-${tray}`).style.width = `${sharePct}%`;
  }
  $("run-name").disabled = Boolean(run) || runSaving;
  $("start-run").hidden = Boolean(run);
  $("end-run").hidden = !run;
  $("end-run").disabled = runSaving;
  connection();
}

async function sendCommand(action, station) {
  const version = ++commandVersion;
  controlStatus = action;
  connection();
  write("command-feedback", reasons.WAITING_ACK);
  try {
    let result = await api("/api/commands", {action, station});
    while (version === commandVersion) {
      write("command-feedback", `${results[result.state]} · ${describe(result.reason)}`);
      if (result.state !== "pending") break;
      await new Promise(resolve => setTimeout(resolve, 250));
      result = await api(`/api/commands/${result.request_id}`);
    }
  } catch (error) {
    if (version === commandVersion) write("command-feedback", error.message === "NETWORK_ERROR"
      ? `Chưa xác nhận · ${describe(error.message)}` : describe(error.message));
  } finally {
    if (version === commandVersion) {
      controlStatus = null;
      connection();
    }
  }
}

const svgNS = "http://www.w3.org/2000/svg";
function svg(tag, attrs, text) {
  const node = document.createElementNS(svgNS, tag);
  for (const [key, val] of Object.entries(attrs)) node.setAttribute(key, val);
  if (text) node.textContent = text;
  return node;
}

let chartData;
function draw(data) {
  chartData = data;
  const chart = $("chart");
  const width = Math.max(280, chart.clientWidth);
  const height = Math.max(170, chart.clientHeight);
  chart.setAttribute("viewBox", `0 0 ${width} ${height}`);
  const {points} = data;
  chart.replaceChildren();
  const real = points.filter(point => !point.gap && trays.some(tray => point.counts[tray] != null));
  if (!real.length) {
    chart.append(svg("text", {x: width / 2, y: height / 2, "text-anchor": "middle", fill: "#4d6172"}, "Đang chờ số đếm đã xác nhận"));
    chart.setAttribute("aria-label", "Chưa có dữ liệu biểu đồ.");
    return;
  }
  chart.setAttribute("aria-label", "Số sản phẩm S, M và L theo lần cập nhật. Chi tiết số đếm có trong CSV.");
  const first = points[0].at;
  const last = points.at(-1).at;
  const end = Math.max(last, first + 1);
  const peak = Math.max(3, ...real.map(point => Math.max(...trays.map(tray => point.counts[tray] || 0))));
  const left = 38;
  const right = width - 35;
  const bottom = height - 37;
  const x = at => left + (at - first) / (end - first) * (right - left);
  const y = n => bottom - n / peak * (bottom - 20);
  const font = {"font-size": 13, fill: "#4d6172"};
  for (const tick of [0, Math.round(peak / 3), Math.round(peak * 2 / 3), peak]) {
    chart.append(svg("line", {x1: left, x2: right, y1: y(tick), y2: y(tick), stroke: "#ccd8e1"}));
    chart.append(svg("text", {x: left - 8, y: y(tick) + 4, "text-anchor": "end", ...font}, numbers.format(tick)));
  }
  const intervals = Math.max(1, Math.min(width < 480 ? 2 : 4, Math.floor(end - first)));
  for (let i = 0; i <= intervals; i++) {
    const at = first + i / intervals * (end - first);
    chart.append(svg("text", {x: x(at), y: bottom + 23, "text-anchor": "middle", ...font}, clock.format(at * 1000)));
  }
  for (const [i, tray] of trays.entries()) {
    let path = "";
    let connected = false;
    const attrs = {stroke: `var(--${tray.toLowerCase()})`, "stroke-width": 2.2, fill: "none",
                   "stroke-dasharray": ["none", "6 3", "2 4"][i]};
    for (const point of points) {
      const n = point.counts[tray];
      if (point.gap || n == null) {
        connected = false;
      } else {
        const position = `${x(point.at).toFixed(2)},${y(n).toFixed(2)}`;
        path += (connected ? "L" : "M") + position;
        if (!connected) chart.append(svg("circle", {cx: x(point.at), cy: y(n), r: 2, fill: attrs.stroke}));
        connected = true;
      }
    }
    chart.append(svg("path", {...attrs, d: path, "vector-effect": "non-scaling-stroke"}));
  }
}

function renderHistory(data) {
  draw(data);
  const rows = $("history-rows");
  const entries = [
    ...data.events.map(entry => [entry.at, kinds[entry.kind], entry.station ? `Trạm ${entry.station} · ${describe(entry.message)}` :
      (entry.kind === "mode_changed" ? modes[entry.message] : describe(entry.message)), value(entry.reference_mm, " mm")]),
    ...data.commands.map(entry => [entry.at, "Lệnh", actionNames[entry.action] + (entry.station ? ` · Trạm ${entry.station}` : ""),
      `${results[entry.state]} · ${describe(entry.reason)}`])
  ].sort((a, b) => b[0] - a[0]).slice(0, 100);
  rows.replaceChildren();
  if (!entries.length) {
    const empty = rows.insertRow().insertCell();
    empty.colSpan = 4;
    empty.textContent = "Chưa có sự kiện trong khoảng đã chọn.";
  }
  for (const [at, ...values] of entries) {
    const row = rows.insertRow();
    for (const item of [dateTime.format(at * 1000), ...values]) row.insertCell().textContent = item;
  }
  write("chart-note", data.points.length === 300
    ? "Hiện tối đa 300 điểm gần nhất. CSV chứa dữ liệu đầy đủ của khoảng đã chọn."
    : "Khoảng trống đánh dấu dữ liệu gián đoạn hoặc chưa xác nhận.");
}

function filter() {
  return selectedRun ? "run_id=" + encodeURIComponent(selectedRun) + "&" : "";
}

let historyVersion = 0;
async function refreshHistory() {
  const version = ++historyVersion;
  const url = `/api/history?${filter()}`;
  $("export").href = url + "format=csv";
  try {
    const [list, data] = await Promise.all([api("/api/runs"), api(url)]);
    if (version !== historyVersion) return;
    const signature = JSON.stringify(list);
    if (signature !== runsSignature) {
      const options = [new Option("5 phút gần nhất", ""), ...list.map(run => new Option(
        `${run.name} · ${dateTime.format(run.started_at * 1000)}`, run.id))];
      $("run-filter").replaceChildren(...options);
      runsSignature = signature;
    }
    $("run-filter").value = selectedRun;
    renderHistory(data);
  } catch (error) {
    if (version === historyVersion) write("chart-note", describe(error.message));
  }
}

async function runAction(url, data, success) {
  if (runSaving) return;
  runSaving = true;
  renderStatus();
  try {
    const run = await api(url, data);
    selectedRun = String(run.id);
    $("run-name").value = "";
    write("run-feedback", success);
    await refreshHistory();
  } catch (error) {
    write("run-feedback", describe(error.message));
  } finally {
    runSaving = false;
    renderStatus();
  }
}

$("run-form").addEventListener("submit", event => {
  event.preventDefault();
  if (isLive() && !state.active_run) runAction("/api/runs", {name: $("run-name").value}, "Đã tạo mốc đầu buổi chạy.");
});
$("end-run").addEventListener("click", () => {
  if (state?.active_run) runAction(`/api/runs/${state.active_run.id}/end`, {}, "Đã kết thúc buổi chạy.");
});
$("run-filter").addEventListener("change", event => {
  selectedRun = event.target.value;
  refreshHistory();
});

let configDirty = false;

function stationH0(index) {
  return state?.snapshot?.[`station_${index}`]?.reference_mm ?? 82;
}

function readConfigInputs() {
  const lMin = Number($("cfg-l-min").value);
  const lMax = Number($("cfg-l-max").value);
  const mMin = Number($("cfg-m-min").value);
  const mMax = Number($("cfg-m-max").value);
  return {
    lMin,
    lMax,
    mMin,
    mMax,
    payload: {
      l_min_mm: Math.round(lMin * 10) / 10,
      l_max_mm: Math.round(lMax * 10) / 10,
      m_min_mm: Math.round(mMin * 10) / 10,
      m_max_mm: Math.round(mMax * 10) / 10,
      station_1_on_ms: Math.round(Number($("cfg-s1-on").value)),
      station_1_delay_ms: 0,
      station_2_on_ms: Math.round(Number($("cfg-s2-on").value)),
      station_2_delay_ms: 0,
      min_off_ms: 120,
    },
  };
}

function updateConfigPreview() {
  const {lMin, lMax, mMin, mMax} = readConfigInputs();
  if (!Number.isFinite(lMin) || !Number.isFinite(lMax) || lMin < 0 || lMin >= lMax || lMax > 300) {
    write("cfg-preview", "Trạm 1 (L): nhập chiều cao hợp lệ 0 ≤ từ < đến ≤ 300 mm.");
    return;
  }
  if (!Number.isFinite(mMin) || !Number.isFinite(mMax) || mMin <= 5 || mMin >= mMax || mMax > 300) {
    write("cfg-preview", "Khoảng M: nhập chiều cao hợp lệ 5 < từ < đến ≤ 300 mm.");
    return;
  }
  const h01 = stationH0(1);
  const h02 = stationH0(2);
  const tofLMin = Math.max(0, Math.round((h01 - lMax) * 10) / 10);
  const tofLMax = Math.max(0, Math.round((h01 - lMin) * 10) / 10);
  let bandNote = "· M đi thẳng cuối băng.";
  if (mMax < lMin) {
    bandNote = `· Khe đệm chống sai số ToF (${numbers.format(mMax)}–${numbers.format(lMin)} mm) đi thẳng không gạt.`;
  } else if (mMax > lMin) {
    bandNote = `· Lưu ý: khoảng M và L đang chồng nhau (${numbers.format(lMin)}–${numbers.format(mMax)} mm) — Trạm 1 ưu tiên gạt L trước.`;
  }
  write(
    "cfg-preview",
    `Trạm 1 gạt L: cao ${numbers.format(lMin)}–${numbers.format(lMax)} mm (ToF ~${numbers.format(tofLMin)}–${numbers.format(tofLMax)} mm theo H0₁=${numbers.format(h01)} mm) · ` +
    `Trạm 2 gạt S: cao từ 5 mm đến dưới ${numbers.format(mMin)} mm theo H0₂=${numbers.format(h02)} mm ${bandNote}`
  );
}

async function loadConfig() {
  try {
    const cfg = await api("/api/config");
    $("cfg-l-min").value = cfg.l_min_mm;
    $("cfg-l-max").value = cfg.l_max_mm;
    $("cfg-m-min").value = cfg.m_min_mm;
    $("cfg-m-max").value = cfg.m_max_mm;
    $("cfg-s1-on").value = cfg.station_1_on_ms;
    $("cfg-s2-on").value = cfg.station_2_on_ms;
    configDirty = false;
    updateConfigPreview();
  } catch (error) {
    write("cfg-feedback", describe(error.message));
  }
}

$("config-form").addEventListener("input", () => {
  configDirty = true;
  updateConfigPreview();
});
$("config-form").addEventListener("submit", async event => {
  event.preventDefault();
  const {lMin, lMax, mMin, mMax, payload} = readConfigInputs();
  if (!Number.isFinite(lMin) || !Number.isFinite(lMax) || lMin < 0 || lMin >= lMax || lMax > 300) {
    write("cfg-feedback", "Lỗi: Khoảng chiều cao Trạm 1 (L) phải thỏa 0 ≤ từ < đến ≤ 300 mm.");
    return;
  }
  if (!Number.isFinite(mMin) || !Number.isFinite(mMax) || mMin < 0 || mMin >= mMax || mMax > 300) {
    write("cfg-feedback", "Lỗi: Khoảng chiều cao Trạm 2 (M) phải thỏa 0 ≤ từ < đến ≤ 300 mm.");
    return;
  }
  $("cfg-save").disabled = true;
  write("cfg-feedback", "Đang lưu cấu hình…");
  try {
    const res = await api("/api/config", payload);
    configDirty = false;
    write(
      "cfg-feedback",
      res.published ? "Đã lưu và áp dụng ngay xuống mạch Pico." : "Đã lưu cấu hình (sẽ tự nạp khi mạch kết nối)."
    );
  } catch (error) {
    write("cfg-feedback", describe(error.message));
  } finally {
    $("cfg-save").disabled = false;
  }
});

async function poll() {
  const started = performance.now();
  try {
    state = await api("/api/status");
    requestOffline = false;
  } catch {
    requestOffline = true;
  }
  renderStatus();
  updateConfigPreview();
  setTimeout(poll, Math.max(150, 1000 - (performance.now() - started)));
}

new ResizeObserver(() => {
  if (chartData) draw(chartData);
}).observe($("chart"));
loadConfig();
poll();
refreshHistory();
async function refreshConsole() {
  try {
    const lines = await api("/api/logs?limit=200");
    const log = $("console-log");
    const follow = log.scrollHeight - log.scrollTop - log.clientHeight < 30;
    log.textContent = lines.length ? lines.map(line => `[${new Date(line.at * 1000).toLocaleString()}] ${line.message}`).join("\n") : "Chưa có log USB.";
    if (follow) log.scrollTop = log.scrollHeight;
  } catch { /* Existing contents remain available while the server reconnects. */ }
}
refreshConsole();
setInterval(refreshConsole, 2000);
setInterval(refreshHistory, 3000);
setInterval(() => {
  if (Boolean(isLive()) !== renderedLive) connection();
}, 150);

