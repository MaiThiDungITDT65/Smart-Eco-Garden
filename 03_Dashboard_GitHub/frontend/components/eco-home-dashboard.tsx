"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  Activity, AlertTriangle, CarFront, CheckCircle2, CloudRain,
  DoorOpen, Droplets, Gauge, Leaf, Lightbulb, Loader2,
  ParkingCircle, RefreshCcw, ShieldAlert, Sprout, Sun, Thermometer, WifiOff, Waves,
} from "lucide-react";

type Mode = "demo" | "live";
type Command = "door/open" | "door/close" | "water/auto" | "water/manual" | "water/on" | "water/off" | "roof/auto" | "roof/manual" | "roof/open" | "roof/close" | "flood/auto" | "flood/manual" | "flood/on" | "flood/off";
type EcoData = {
  roofAngleReported?: boolean;
  roofAngle?: number | null;
  roofTargetAngle?: number | null;
  roofCoverRequested?: boolean;
  roofState?: string;
  roofWeather?: string;
  roofMode?: "AUTO" | "MANUAL" | "UNKNOWN";
  floodMode?: "AUTO" | "MANUAL" | "UNKNOWN";
  manualModesReported?: boolean;
  eventQueue?: number | null;

  temperature: number | null;
  humidity: number | null;
  soilRaw: number | null;
  lightRaw: number | null;
  rainRaw: number | null;
  raining: boolean;
  vibration: boolean;
  floodRaw: number | null;
  floodAlert: boolean;
  pump1: boolean;
  pump2: boolean | null;
  waterMode: "AUTO" | "MANUAL" | "UNKNOWN";
  doorOpen: boolean;
  distanceCm: number | null;
  carDetected: boolean;
  unoConnected: boolean;
  ledRed: boolean;
  ledGreen: boolean;
  ledYellow: boolean;
  soilStatus: string;
};
type ChartSample = { t: number; temperature?: number | null; soilRaw?: number | null; floodRaw?: number | null };
type EcoSnapshot = {
  mode: Mode;
  simulated: boolean;
  connected: boolean;
  error: string | null;
  timestamp: number;
  data: EcoData | null;
  history: ChartSample[];
};

type SensorDraft = { floodRaw: number; soil: number; rain: number; distance: number; vibration: boolean };
const DEFAULT_DRAFT: SensorDraft = { floodRaw: 0, soil: 510, rain: 1020, distance: 16, vibration: false };
const API_ROOT = ""; // SAME-ORIGIN: Next.js rewrites /api/eco to laptop Backend

async function api<T>(path: string, method: "GET" | "POST" = "GET", value?: unknown): Promise<T> {
  const response = await fetch(`${API_ROOT}${path}`, {
    method,
    headers: { "Content-Type": "application/json" },
    body: value === undefined ? undefined : JSON.stringify(value),
    cache: "no-store",
  });
  let result: unknown;
  try { result = await response.json(); } catch { throw new Error(`Máy chủ trả về dữ liệu không hợp lệ (${response.status})`); }
  if (!response.ok) {
    const error = result as { detail?: string };
    throw new Error(error.detail || `HTTP ${response.status}`);
  }
  return result as T;
}

function fmt(value: number | null | undefined, decimals = 0): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "--";
  return value.toLocaleString("vi-VN", { maximumFractionDigits: decimals, minimumFractionDigits: decimals });
}
function StatusPill({ children, color = "neutral" }: { children: React.ReactNode; color?: "neutral" | "green" | "red" | "yellow" }) {
  const colors = {
    neutral: "border-stone-200 bg-stone-50 text-stone-600",
    green: "border-emerald-200 bg-emerald-50 text-emerald-800",
    red: "border-rose-200 bg-rose-50 text-rose-800",
    yellow: "border-amber-200 bg-amber-50 text-amber-800",
  };
  return <span className={`inline-flex items-center gap-1 rounded-full border px-2.5 py-1 text-[11px] font-semibold ${colors[color]}`}>{children}</span>;
}
function Metric({ label, value, unit, foot, icon: Icon, accent = "emerald" }: {
  label: string; value: string; unit?: string; foot?: string; icon: React.ComponentType<{ className?: string }>;
  accent?: "emerald" | "blue" | "amber" | "rose";
}) {
  const colors = { emerald: "bg-emerald-50 text-emerald-700", blue: "bg-sky-50 text-sky-700", amber: "bg-amber-50 text-amber-700", rose: "bg-rose-50 text-rose-700" };
  return <div className="rounded-2xl border border-stone-200 bg-white p-4 shadow-sm">
    <div className="flex items-center justify-between gap-2"><span className="text-xs font-medium text-stone-500">{label}</span><span className={`rounded-xl p-2 ${colors[accent]}`}><Icon className="h-4 w-4" /></span></div>
    <div className="mt-3 flex items-baseline gap-1.5"><span className="text-2xl font-bold tracking-tight text-stone-900">{value}</span>{unit && <span className="text-xs text-stone-500">{unit}</span>}</div>
    <p className="mt-1 min-h-4 text-[11px] text-stone-500">{foot || "Chưa có dữ liệu"}</p>
  </div>;
}
function Panel({ title, icon: Icon, note, children }: {
  title: string; icon: React.ComponentType<{ className?: string }>; note?: string; children: React.ReactNode;
}) {
  return <section className="rounded-2xl border border-stone-200 bg-white p-5 shadow-sm">
    <div className="flex items-start gap-2.5 border-b border-stone-100 pb-3"><div className="rounded-lg bg-emerald-50 p-2 text-emerald-700"><Icon className="h-4 w-4" /></div><div><h2 className="text-sm font-bold text-stone-900">{title}</h2>{note && <p className="mt-1 text-xs leading-5 text-stone-500">{note}</p>}</div></div>
    <div className="pt-4">{children}</div>
  </section>;
}
function ActionButton({ label, onClick, disabled, kind = "primary", busy }: {
  label: string; onClick: () => void; disabled?: boolean; kind?: "primary" | "plain" | "danger"; busy?: boolean;
}) {
  const styles = {
    primary: "bg-emerald-700 text-white hover:bg-emerald-800",
    plain: "border border-stone-200 bg-stone-50 text-stone-800 hover:bg-stone-100",
    danger: "border border-rose-200 bg-rose-50 text-rose-700 hover:bg-rose-100",
  };
  return <button type="button" onClick={onClick} disabled={disabled || busy} className={`inline-flex min-h-10 items-center justify-center gap-2 rounded-xl px-3 py-2 text-xs font-bold transition disabled:cursor-not-allowed disabled:opacity-45 ${styles[kind]}`}>
    {busy && <Loader2 className="h-3.5 w-3.5 animate-spin" />}{label}
  </button>;
}
function Lamp({ color, on, label }: { color: "red" | "green" | "yellow"; on: boolean | null; label: string }) {
  const onColors = { red: "bg-red-500 shadow-[0_0_16px_#f87171]", green: "bg-emerald-500 shadow-[0_0_16px_#34d399]", yellow: "bg-amber-400 shadow-[0_0_16px_#fbbf24]" };
  return <div className="flex items-center gap-2 rounded-xl border border-stone-100 bg-stone-50 px-3 py-3"><span className={`h-4 w-4 shrink-0 rounded-full border border-black/10 ${on ? onColors[color] : "bg-stone-200"}`} /><span className="text-xs font-semibold text-stone-700">{label}</span><span className="ml-auto text-xs text-stone-500">{on === null ? "--" : on ? "Sáng" : "Tắt"}</span></div>;
}
function LineChart({ history }: { history: ChartSample[] }) {
  const width = 800, height = 160;
  const points = (key: "soilRaw" | "floodRaw" | "temperature") => history
    .map((r, i) => {
      const v = r[key];
      if (v === null || v === undefined || !Number.isFinite(v)) return null;
      const x = 10 + (i / Math.max(1, history.length - 1)) * (width - 20);
      const scaled = key === "temperature" ? v * 20 : v;
      const y = height - 10 - Math.max(0, Math.min(1200, scaled)) / 1200 * (height - 20);
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).filter((x): x is string => x !== null).join(" ");
  const haveData = history.length > 1;
  return <div>
    <div className="flex flex-wrap gap-4 pb-2 text-[11px] text-stone-500"><span className="flex items-center gap-1.5"><span className="h-2.5 w-2.5 rounded-full bg-emerald-600" />Độ ẩm đất</span><span className="flex items-center gap-1.5"><span className="h-2.5 w-2.5 rounded-full bg-rose-600" />Mực nước</span><span className="flex items-center gap-1.5"><span className="h-2.5 w-2.5 rounded-full bg-amber-600" />Nhiệt độ (tỷ lệ hiển thị)</span></div>
    <div className="overflow-hidden rounded-xl border border-stone-100 bg-stone-50 p-2"><svg viewBox={`0 0 ${width} ${height}`} className="h-44 w-full" role="img" aria-label="Biểu đồ xu hướng các cảm biến">
      {[0, 1, 2, 3, 4].map(i => <line key={i} x1="0" y1={8 + i * 36} x2={width} y2={8 + i * 36} stroke="#e7e5e4" strokeWidth="1" />)}
      {haveData && <><polyline points={points("soilRaw")} stroke="#059669" strokeWidth="2.4" fill="none" strokeLinejoin="round" strokeLinecap="round" /><polyline points={points("floodRaw")} stroke="#e11d48" strokeWidth="2.4" fill="none" strokeLinejoin="round" strokeLinecap="round" /><polyline points={points("temperature")} stroke="#d97706" strokeWidth="2.4" fill="none" strokeLinejoin="round" strokeLinecap="round" /></>}
      {!haveData && <text x="400" y="80" fill="#78716c" textAnchor="middle" fontSize="16">Đang thu thập dữ liệu biểu đồ...</text>}
    </svg></div><p className="mt-2 text-[11px] text-stone-500">Biểu đồ chỉ lưu ngắn hạn trong RAM Backend. Không quy đổi ADC sang % nếu chưa hiệu chuẩn.</p>
  </div>;
}

export default function EcoHomeDashboard() {
  const [snapshot, setSnapshot] = useState<EcoSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState("Hệ thống đã sẵn sàng.");
  const [busy, setBusy] = useState<string | null>(null);
  const [draft, setDraft] = useState<SensorDraft>(DEFAULT_DRAFT);
  const loadingRef = useRef(false);
  const mode = snapshot?.mode || "demo";
  const data = snapshot?.data || null;
  const canControl = !!snapshot && (mode === "demo" || snapshot.connected);

  const refresh = useCallback(async () => {
    if (loadingRef.current) return;
    loadingRef.current = true;
    try {
      const status = await api<EcoSnapshot>("/api/eco/status");
      setSnapshot(status);
      setError(null);
    } catch (e) { setError(e instanceof Error ? e.message : "Không thể kết nối Backend"); }
    finally { loadingRef.current = false; }
  }, []);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), 1500);
    return () => clearInterval(timer);
  }, [refresh]);

  async function command(cmd: Command) {
    if (!canControl || busy) return;
    setBusy(cmd);
    try {
      const result = await api<{ simulated: boolean; note?: string }>("/api/eco/command", "POST", { command: cmd });
      const commandName: Record<Command, string> = {
        "door/open": "Mở cửa", "door/close": "Đóng cửa",
        "water/auto": "Chuyển tưới tự động", "water/manual": "Chuyển tưới thủ công",
        "water/on": "Bật tưới", "water/off": "Tắt tưới",
        "roof/auto": "Mái che tự động", "roof/manual": "Mái che thủ công",
        "roof/open": "Mở mái", "roof/close": "Đóng mái",
        "flood/auto": "Chống ngập tự động", "flood/manual": "Chống ngập thủ công",
        "flood/on": "Bật bơm chống ngập", "flood/off": "Tắt bơm chống ngập",
      };
      setNotice(`${result.simulated ? "Mô phỏng" : "Đã gửi yêu cầu"}: ${commandName[cmd]}.`);
      await refresh();
    } catch (e) { setNotice("LỖI LỆNH: " + (e instanceof Error ? e.message : "Không xác định")); }
    finally { setBusy(null); }
  }
  async function setMode(next: Mode) {
    if (busy || next === mode) return;
    setBusy("mode");
    try {
      await api("/api/eco/mode", "POST", { mode: next });
      setNotice(next === "demo" ? "Đã bật mô phỏng. Không gửi lệnh tới thiết bị." : "Đã chọn thiết bị thật. Đang kiểm tra kết nối ESP32...");
      await refresh();
    } catch (e) { setNotice("Không đổi được chế độ: " + (e instanceof Error ? e.message : "Lỗi")); }
    finally { setBusy(null); }
  }
  async function applySensors(values: SensorDraft) {
    if (mode !== "demo" || busy) return;
    setBusy("sensors");
    try {
      await api("/api/eco/demo/sensors", "POST", values);
      setNotice("Đã áp dụng tình huống mô phỏng. Không tác động tới mô hình thật.");
      await refresh();
    } catch (e) { setNotice("LỖI MÔ PHỎNG: " + (e instanceof Error ? e.message : "Không xác định")); }
    finally { setBusy(null); }
  }
  const s = (v: string) => data ? v : "--";

  return <div className="min-h-screen bg-[#f5f7f4] text-stone-900">
    <header className="border-b border-emerald-900/10 bg-white">
      <div className="mx-auto flex w-full max-w-7xl flex-wrap items-center justify-between gap-3 px-4 py-4 sm:px-6">
        <div className="flex items-center gap-3"><div className="rounded-xl bg-emerald-800 p-2.5 text-white"><Leaf className="h-5 w-5" /></div><div><h1 className="text-base font-bold">Smart Eco Garden</h1><p className="text-[11px] text-stone-500">Giám sát và điều khiển vườn thông minh</p></div></div>
        <div className="flex flex-wrap items-center gap-2"><a href="/history" className="rounded-xl border border-emerald-200 bg-emerald-50 px-3 py-2 text-xs font-semibold text-emerald-800">Lịch sử hoạt động</a><StatusPill color={mode === "demo" ? "yellow" : snapshot?.connected ? "green" : "red"}>{mode === "demo" ? "Chế độ mô phỏng" : snapshot?.connected ? "Đã kết nối thiết bị" : "Chưa kết nối thiết bị"}</StatusPill><button type="button" onClick={() => void refresh()} className="rounded-lg border border-stone-200 bg-white p-2 hover:bg-stone-50" aria-label="Làm mới"><RefreshCcw className="h-4 w-4" /></button></div>
      </div>
    </header>

    <main className="mx-auto max-w-7xl space-y-5 px-4 py-5 sm:px-6">
      <div className={`flex items-start gap-3 rounded-xl border p-4 ${mode === "demo" ? "border-amber-200 bg-amber-50 text-amber-900" : snapshot?.connected ? "border-emerald-200 bg-emerald-50 text-emerald-900" : "border-rose-200 bg-rose-50 text-rose-900"}`}>
        {mode === "demo" ? <AlertTriangle className="mt-0.5 h-5 w-5 shrink-0" /> : snapshot?.connected ? <CheckCircle2 className="mt-0.5 h-5 w-5 shrink-0" /> : <WifiOff className="mt-0.5 h-5 w-5 shrink-0" />}
        <div className="flex-1"><p className="text-sm font-bold">{mode === "demo" ? "Đang sử dụng dữ liệu mô phỏng" : snapshot?.connected ? "Đã kết nối mô hình Smart Eco Garden" : "Không có kết nối tới ESP32"}</p><p className="mt-1 text-xs leading-5">{mode === "demo" ? "Các thao tác chỉ thay đổi dữ liệu mô phỏng, không ảnh hưởng thiết bị." : snapshot?.connected ? "Dữ liệu đang được cập nhật từ mô hình. Các thao tác điều khiển sẽ tác động đến thiết bị thật." : "Thiết bị chưa kết nối. Các nút điều khiển bị vô hiệu hóa để tránh hiểu nhầm."}</p></div>
        <div className="flex shrink-0 flex-wrap gap-1 rounded-lg border border-stone-200 bg-white/80 p-1"><button type="button" onClick={() => void setMode("demo")} disabled={!!busy} className={`rounded-md px-2.5 py-1.5 text-xs font-semibold ${mode === "demo" ? "bg-emerald-800 text-white" : "text-stone-600"}`}>Mô phỏng</button><button type="button" onClick={() => void setMode("live")} disabled={!!busy} className={`rounded-md px-2.5 py-1.5 text-xs font-semibold ${mode === "live" ? "bg-emerald-800 text-white" : "text-stone-600"}`}>Thiết bị thật</button></div>
      </div>
      {error && <div role="alert" className="rounded-xl border border-rose-200 bg-rose-50 px-4 py-3 text-sm text-rose-800"><strong>Lỗi kết nối Backend:</strong> {error}. Hãy kiểm tra PowerShell chạy FastAPI tại cổng 8000.</div>}

      
      {mode === "live" && data && <div className="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-emerald-100 bg-emerald-50 px-4 py-3 text-xs text-emerald-950"><span>Sự kiện đang chờ lưu: <strong>{data.eventQueue ?? "--"}</strong></span><a href="/history" className="font-semibold underline underline-offset-2">Mở lịch sử hoạt động</a></div>}
      <div className="flex items-center justify-between"><h2 className="text-xs font-bold uppercase tracking-widest text-stone-500">Tổng quan hệ thống</h2><span className="text-[11px] text-stone-400">Tự cập nhật mỗi 1,5 giây</span></div>
      <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Metric label="Nhiệt độ" value={fmt(data?.temperature, 1)} unit="°C" foot="Cảm biến nhiệt độ" icon={Thermometer} />
        <Metric label="Độ ẩm không khí" value={fmt(data?.humidity, 1)} unit="%" foot="DHT11" icon={Droplets} accent="blue" />
        <Metric label="Độ ẩm đất" value={fmt(data?.soilRaw)} unit="ADC" foot={data?.soilStatus || "Chưa có dữ liệu"} icon={Sprout} accent="emerald" />
        <Metric label="Ánh sáng" value={fmt(data?.lightRaw)} unit="ADC" foot="Cường độ ánh sáng" icon={Sun} accent="amber" />
        <Metric label="Cảm biến mưa" value={fmt(data?.rainRaw)} unit="ADC" foot={data?.rainRaw == null ? "Chưa có dữ liệu" : data.raining ? "Đang mưa" : "Không mưa"} icon={CloudRain} accent="blue" />
        <Metric label="Mực nước ngập" value={fmt(data?.floodRaw)} unit="ADC" foot={data ? data.ledRed ? "ĐANG CẢNH BÁO" : "Đang giám sát" : "Chưa có dữ liệu"} icon={Waves} accent={data?.ledRed ? "rose" : "blue"} />
        <Metric label="Khoảng cách xe" value={fmt(data?.distanceCm, 1)} unit="cm" foot={data ? data.carDetected ? "Có xe đỗ" : "Chỗ trống" : "Chưa có dữ liệu"} icon={CarFront} accent="amber" />
        <Metric label="Cảm biến rung" value={s(data?.vibration ? "Có rung" : "Bình thường")} foot="Giám sát rung động" icon={ShieldAlert} accent={data?.vibration ? "rose" : "emerald"} />
      </div>

      <div className="grid gap-4 lg:grid-cols-2">
        {/* GARDEN_ROOF_PANEL_V3: Do not assume commanded servo angles are measured angles. */}
        <Panel title="Mái che nhà kính" icon={CloudRain} note="Điều khiển tự động hoặc thủ công">
          <div className="grid gap-3 sm:grid-cols-2">
            <div className="rounded-xl bg-stone-50 p-3">
              <p className="text-xs text-stone-500">Điều kiện thời tiết</p>
              <p className="mt-2 text-base font-bold">{data?.rainRaw == null ? "--" : data.raining ? "ĐANG MƯA" : "KHÔ RÁO"}</p>
              <StatusPill color={data?.raining ? "yellow" : "green"}>{data ? `RAW ${fmt(data.rainRaw)}` : "Chưa có dữ liệu"}</StatusPill>
            </div>
            <div className="rounded-xl bg-stone-50 p-3">
              <p className="text-xs text-stone-500">Trạng thái mái che</p>
              <p className="mt-2 text-base font-bold">{!data?.roofAngleReported ? "CHƯA CÓ DỮ LIỆU" : data.roofState === "MOVING" ? "ĐANG DI CHUYỂN" : data.roofState === "COVERED" ? "ĐÃ RA LỆNH CHE MƯA" : data.roofState === "RETRACTED" ? "ĐÃ RA LỆNH THU GỌN" : "KHÔNG XÁC ĐỊNH"}</p>
              <StatusPill color={data?.roofState === "MOVING" ? "yellow" : data?.roofAngleReported ? "green" : "neutral"}>{data?.roofAngleReported ? "Đã nhận dữ liệu" : "Chưa có dữ liệu"}</StatusPill>
            </div>
          </div>
          <div className="mt-4 rounded-xl border border-stone-100 p-3">
            <div className="flex flex-wrap items-center justify-between gap-2 text-sm">
              <span className="text-stone-600">Góc lệnh hiện tại <strong className="text-stone-900">{data?.roofAngleReported ? fmt(data.roofAngle) + "°" : "--"}</strong></span>
              <span className="text-stone-600">Góc đích <strong className="text-stone-900">{data?.roofAngleReported ? fmt(data.roofTargetAngle) + "°" : "--"}</strong></span>
            </div>
            <div className="mt-3 h-3 overflow-hidden rounded-full bg-stone-100"><div className="h-full rounded-full bg-emerald-600 transition-[width] duration-300" style={{ width: `${data?.roofAngleReported && data.roofAngle != null ? Math.min(100,Math.max(0,data.roofAngle/180*100)) : 0}%` }} /></div>
          </div>
          <div className="mt-4 flex items-center justify-between gap-2 text-sm"><span>Chế độ mái che</span><strong>{data?.roofMode || "--"}</strong></div>
          <div className="mt-2 grid grid-cols-2 gap-2 sm:grid-cols-4">
            <ActionButton label="AUTO" onClick={() => void command("roof/auto")} disabled={!canControl || !data?.manualModesReported} busy={busy === "roof/auto"} kind={data?.roofMode === "AUTO" ? "primary" : "plain"}/>
            <ActionButton label="MANUAL" onClick={() => void command("roof/manual")} disabled={!canControl || !data?.manualModesReported} busy={busy === "roof/manual"} kind={data?.roofMode === "MANUAL" ? "primary" : "plain"}/>
            <ActionButton label="Mở mái" onClick={() => void command("roof/open")} disabled={!canControl || data?.roofMode !== "MANUAL"} busy={busy === "roof/open"}/>
            <ActionButton label="Đóng mái" onClick={() => void command("roof/close")} disabled={!canControl || data?.roofMode !== "MANUAL"} busy={busy === "roof/close"} kind="plain"/>
          </div>
          <p className="mt-3 text-xs leading-5 text-stone-500">Ở chế độ tự động, mái che dựa vào cảm biến mưa. Chuyển sang thủ công để mở hoặc đóng mái. Góc hiển thị là góc lệnh, không phải số đo vị trí cơ khí.</p>
        </Panel>

        <Panel title="Hệ thống tưới cây" note="Tưới tự động hoặc điều khiển thủ công" icon={Sprout}>
          <div className="flex flex-wrap items-center justify-between gap-3"><div><p className="text-xs text-stone-500">Chế độ</p><p className="mt-1 text-lg font-bold">{data?.waterMode || "--"}</p></div><div className="text-right"><p className="text-xs text-stone-500">Trạng thái bơm 1</p><StatusPill color={data?.pump1 ? "green" : "neutral"}>{data ? data.pump1 ? "ĐANG TƯỚI" : "ĐÃ TẮT" : "CHƯA CÓ DỮ LIỆU"}</StatusPill></div></div>
          <div className="mt-4 grid grid-cols-2 gap-2 sm:grid-cols-4"><ActionButton label="AUTO" onClick={() => void command("water/auto")} disabled={!canControl} busy={busy === "water/auto"} kind={data?.waterMode === "AUTO" ? "primary" : "plain"} /><ActionButton label="MANUAL" onClick={() => void command("water/manual")} disabled={!canControl} busy={busy === "water/manual"} kind={data?.waterMode === "MANUAL" ? "primary" : "plain"} /><ActionButton label="Bật tưới" onClick={() => void command("water/on")} disabled={!canControl || data?.waterMode !== "MANUAL"} busy={busy === "water/on"} /><ActionButton label="Tắt tưới" onClick={() => void command("water/off")} disabled={!canControl} busy={busy === "water/off"} kind="danger" /></div>
          <p className="mt-3 text-[11px] leading-5 text-stone-500">Tưới tự động khi đất khô và trời không mưa. Mỗi lần tưới tối đa 5 giây.</p>
        </Panel>
        <Panel title="Cửa thông minh" note="Mở bằng bàn phím hoặc Dashboard; tự đóng sau 5 giây" icon={DoorOpen}>
          <div className="flex flex-wrap items-center justify-between gap-2"><div><p className="text-xs text-stone-500">Trạng thái cửa</p><p className="mt-1 text-lg font-bold">{data ? data.doorOpen ? "ĐANG MỞ" : "ĐÃ ĐÓNG" : "--"}</p></div><StatusPill color={data?.doorOpen ? "yellow" : "green"}>{data ? data.doorOpen ? "Cửa đang mở" : "Cửa an toàn" : "Chưa có dữ liệu"}</StatusPill></div>
          <div className="mt-5 grid grid-cols-2 gap-2"><ActionButton label="Mở cửa" onClick={() => void command("door/open")} disabled={!canControl} busy={busy === "door/open"} /><ActionButton label="Đóng cửa" onClick={() => void command("door/close")} disabled={!canControl} busy={busy === "door/close"} kind="plain" /></div>
          <p className="mt-3 text-[11px] text-stone-500">Trạng thái hiển thị theo dữ liệu thiết bị báo về.</p>
        </Panel>
        <Panel title="Hệ thống chống ngập" note="Theo dõi mực nước và điều khiển bơm" icon={Waves}>
          <div className="grid grid-cols-2 gap-3"><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">Mực nước RAW</p><p className="mt-2 text-xl font-bold">{fmt(data?.floodRaw)}</p><StatusPill color={data?.ledRed ? "red" : "green"}>{data ? data.ledRed ? "CẢNH BÁO NGẬP" : "Bình thường" : "Chưa có dữ liệu"}</StatusPill></div><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">Bơm chống ngập</p><p className="mt-2 text-xl font-bold">{data?.pump2 === null || !data ? "--" : data.pump2 ? "ĐANG CHẠY" : "ĐÃ TẮT"}</p><p className="mt-2 text-[11px] text-stone-500">{mode === "live" ? "Trạng thái bơm từ thiết bị" : "Trạng thái mô phỏng"}</p></div></div>
          <div className="mt-4 flex items-center justify-between gap-2 text-sm"><span>Chế độ chống ngập</span><strong>{data?.floodMode || "--"}</strong></div>
          <div className="mt-2 grid grid-cols-2 gap-2 sm:grid-cols-4">
            <ActionButton label="AUTO" onClick={() => void command("flood/auto")} disabled={!canControl || !data?.manualModesReported} busy={busy === "flood/auto"} kind={data?.floodMode === "AUTO" ? "primary" : "plain"}/>
            <ActionButton label="MANUAL" onClick={() => void command("flood/manual")} disabled={!canControl || !data?.manualModesReported} busy={busy === "flood/manual"} kind={data?.floodMode === "MANUAL" ? "primary" : "plain"}/>
            <ActionButton label="Bật bơm" onClick={() => void command("flood/on")} disabled={!canControl || data?.floodMode !== "MANUAL" || (data?.floodRaw ?? 0) <= 100} busy={busy === "flood/on"}/>
            <ActionButton label="Tắt bơm" onClick={() => void command("flood/off")} disabled={!canControl || data?.floodMode !== "MANUAL"} busy={busy === "flood/off"} kind="danger"/>
          </div>
          <p className="mt-3 text-[11px] leading-5 text-stone-500">Chế độ thủ công chỉ bật được bơm khi cảm biến mực nước có tín hiệu hợp lệ và chưa quá thấp. Bơm tự ngắt sau tối đa 30 giây; muốn chạy lần nữa cần Tắt rồi Bật lại.</p>
        </Panel>
        <Panel title="Bãi đỗ xe tự động" note="Phát hiện xe tại vị trí đỗ" icon={ParkingCircle}>
          <div className="flex items-center justify-between"><div><p className="text-xs text-stone-500">Khoảng cách đo</p><p className="mt-1 text-2xl font-bold">{fmt(data?.distanceCm, 1)} <span className="text-sm font-normal text-stone-500">cm</span></p></div><StatusPill color={data?.carDetected ? "red" : "green"}>{data ? data.carDetected ? "CÓ XE" : "CHỖ TRỐNG" : "Chưa có dữ liệu"}</StatusPill></div>
          <p className="mt-4 text-[11px] leading-5 text-stone-500">Đèn báo đỏ khi có xe, xanh khi chỗ đỗ trống, vàng khi không đo được.</p>
        </Panel>
        <Panel title="Đèn trạng thái" note="Theo dõi cảnh báo và trạng thái vận hành" icon={Lightbulb}>
          <div className="grid gap-2"><Lamp color="red" label="Cảnh báo ngập" on={data ? data.ledRed : null} /><Lamp color="green" label="Hệ thống bình thường" on={data ? data.ledGreen : null} /><Lamp color="yellow" label="Đang tưới cây" on={data ? data.ledYellow : null} /></div>
          <p className="mt-3 text-[11px] text-stone-500">Các đèn phản ánh trạng thái hệ thống; chưa có cảm biến đo trạng thái đèn trực tiếp.</p>
        </Panel>
        <Panel title="An ninh & kết nối hệ thống" note="Giám sát rung động và kết nối thiết bị" icon={ShieldAlert}>
          <div className="grid gap-3 sm:grid-cols-2"><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">Cảm biến rung</p><p className="mt-2 text-base font-bold">{data ? data.vibration ? "PHÁT HIỆN RUNG" : "Bình thường" : "--"}</p></div><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">Arduino UNO</p><p className="mt-2 text-base font-bold">{data ? mode === "demo" ? "Mô phỏng" : data.unoConnected ? "Đã kết nối" : "Mất kết nối" : "--"}</p></div><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">ESP32</p><p className="mt-2 text-base font-bold">{mode === "demo" ? "Mô phỏng" : snapshot?.connected ? "Trực tuyến" : "Ngoại tuyến"}</p></div><div className="rounded-xl bg-stone-50 p-3"><p className="text-xs text-stone-500">Máy chủ Dashboard</p><p className="mt-2 text-base font-bold">{error ? "Mất kết nối" : "Đã kết nối"}</p></div></div>
        </Panel>
      </div>

      {mode === "demo" && <Panel title="Thử nghiệm mô phỏng" icon={Gauge} note="Điều chỉnh giá trị để kiểm tra logic khi chưa kết nối mô hình">
        <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
          {([ ["floodRaw", "Mực nước RAW", 0, 1200, 25], ["soil", "Độ ẩm đất RAW", 0, 1023, 10], ["rain", "Cảm biến mưa RAW", 0, 1023, 10], ["distance", "Khoảng cách xe (cm)", 2, 40, 1] ] as const).map(([key,label,min,max,step]) => <label key={key} className="rounded-xl border border-stone-100 bg-stone-50 p-3"><span className="flex items-center justify-between gap-2 text-xs font-semibold"><span>{label}</span><span className="text-emerald-700">{draft[key]}</span></span><input className="mt-4 w-full accent-emerald-700" type="range" min={min} max={max} step={step} value={draft[key]} onChange={e => setDraft(v => ({ ...v, [key]: Number(e.target.value) }))} /></label>)}
        </div>
        <label className="mt-3 flex cursor-pointer items-center gap-2 rounded-xl border border-stone-100 bg-stone-50 p-3 text-xs font-semibold"><input type="checkbox" checked={draft.vibration} onChange={e => setDraft(v => ({...v, vibration:e.target.checked}))} /> Giả lập rung động</label>
        <div className="mt-4 flex flex-wrap gap-2"><ActionButton label="Áp dụng tình huống" onClick={() => void applySensors(draft)} disabled={!!busy} busy={busy === "sensors"} /><ActionButton label="Khôi phục cảm biến" onClick={() => { setDraft(DEFAULT_DRAFT); void applySensors(DEFAULT_DRAFT); }} disabled={!!busy} kind="plain" /></div>
        <p className="mt-3 text-[11px] text-stone-500">Ví dụ: ngập RAW=900 rồi đợi 3 giây → bơm 2 bật. Giảm về 500 → cảnh báo vẫn giữ. Giảm về 0 và đợi 3 giây → cảnh báo tắt. Ở chế độ mô phỏng tuyệt đối không phát lệnh ra thiết bị.</p>
      </Panel>}

      <Panel title="Biểu đồ cảm biến theo thời gian" icon={Activity} note="Dữ liệu cập nhật tự động từ Backend"><LineChart history={snapshot?.history || []} /></Panel>
      <div aria-live="polite" className="rounded-xl border border-stone-200 bg-white px-4 py-3 text-xs text-stone-600"><strong>Thông báo:</strong> {notice}</div>
      <footer className="flex flex-wrap items-center justify-between gap-3 border-t border-stone-200 py-4 text-[11px] text-stone-500"><span>Smart Eco Garden · Hệ thống giám sát và điều khiển</span><div className="flex items-center gap-3"><span>Chỉ sử dụng trên mạng LAN riêng tin cậy</span></div></footer>
    </main>
  </div>;
}
