"""Smart Eco Garden event/sensor history for the existing FastAPI backend.

LIVE-only collection; durable SQLite; best-effort Sheets synchronization.
No Google credentials needed for local operation.
"""
from __future__ import annotations

import csv
import io
import json
import os
import sqlite3
import threading
import time
from datetime import datetime, timedelta
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Callable
from urllib.parse import quote

from fastapi import APIRouter, Query
from fastapi.responses import StreamingResponse

router = APIRouter()
BASE = Path(__file__).resolve().parent.parent
STORE = BASE / "garden_data"
DB = STORE / "eco_history.sqlite3"
CONFIG_FILE = BASE / "eco_sheets_config.json"
LOCK = threading.RLock()
LAST_SAMPLE = 0.0
LAST_SYNC = 0.0
SYNC_BUSY = False
LAST_SYNC_ERROR: str | None = None

# Không có cột UID: hệ thống dùng mã Keypad chung.
# Event ID chỉ dùng nội bộ để nhận biết bản ghi và tránh đồng bộ trùng.
HEADERS_EVENTS = [
    "TimeStamp", "Time", "Date", "Thiết bị", "Hoạt động", "Nguồn", "Mã sự kiện",
]

HEADERS_SENSORS = [
    "TimeStamp", "Time", "Date",
    "Nhiệt độ (°C)", "Độ ẩm không khí (%)", "Độ ẩm đất (RAW)",
    "Ánh sáng (RAW)", "Mưa (RAW)", "Mực nước (RAW)",
    "Khoảng cách (cm)", "Rung", "Bơm tưới", "Bơm chống ngập",
    "Góc mái đã ra lệnh (°)", "Góc mái mục tiêu (°)",
    "Chế độ mái", "Chế độ chống ngập", "Mã mẫu",
]


def date_columns(timestamp: str) -> tuple[str, str, str]:
    """Chuyển timestamp ISO lưu trong SQLite sang dạng giống mẫu Google Sheets."""
    dt = datetime.fromisoformat(str(timestamp).replace("Z", "+00:00"))
    if dt.tzinfo is None:
        dt = dt.astimezone()
    return (dt.strftime("%d/%m/%Y %H:%M:%S"),
            dt.strftime("%H:%M:%S"), dt.strftime("%d/%m/%Y"))


def event_sheet_row(record: dict | sqlite3.Row) -> list:
    timestamp, clock, date = date_columns(record["ts"])
    return [timestamp, clock, date, record["device"],
            record["details"], record["source"], record["uid"]]


def sensor_sheet_row(record: dict | sqlite3.Row) -> list:
    timestamp, clock, date = date_columns(record["ts"])
    data = json.loads(record["payload"])
    return [
        timestamp, clock, date,
        data.get("temperature"), data.get("humidity"), data.get("soil"),
        data.get("light"), data.get("rain"), data.get("floodRaw"),
        data.get("distance"), data.get("vibration"), data.get("pump1"),
        data.get("pump2"), data.get("roofAngle"), data.get("roofTargetAngle"),
        data.get("roofMode"), data.get("floodMode"), "S-" + str(record["id"]),
    ]

EVENT_LABELS = {
    "DOOR_OPEN": "Đã gửi lệnh mở cửa", "DOOR_CLOSE": "Đã gửi lệnh đóng cửa",
    "ACCESS_DENIED": "Nhập sai mật khẩu Keypad", "WATER_CMD": "Đã gửi lệnh tưới cây",
    "PUMP1_ON": "Bơm tưới bật", "PUMP1_OFF": "Bơm tưới tắt",
    "PUMP2_ON": "Bơm chống ngập bật", "PUMP2_OFF": "Bơm chống ngập tắt",
    "ROOF_COVER": "Mái nhận lệnh che mưa 90°", "ROOF_RETRACT": "Mái nhận lệnh thu gọn 180°",
    "ROOF_REACHED": "Đã gửi đủ lệnh góc mái mục tiêu",
    "ROOF_AUTO": "Mái che chuyển AUTO", "ROOF_MANUAL": "Mái che chuyển MANUAL",
    "FLOOD_AUTO": "Chống ngập chuyển AUTO", "FLOOD_MANUAL": "Chống ngập chuyển MANUAL",
    "SYSTEM_CMD": "Lệnh điều khiển từ Dashboard",
    "RAIN_ON": "Cảm biến phát hiện mưa", "RAIN_OFF": "Cảm biến hết mưa",
    "FLOOD_ON": "Bắt đầu cảnh báo ngập", "FLOOD_OFF": "Đã xóa cảnh báo ngập",
    "CAR_IN": "Có xe vào vị trí đỗ", "CAR_OUT": "Xe rời vị trí đỗ",
    "VIBRATION": "Phát hiện rung", "MODE_AUTO": "Chế độ tưới AUTO", "MODE_MANUAL": "Chế độ tưới MANUAL",
    "ESP_BOOT": "ESP32 khởi động", "UNO_ONLINE": "UNO có dữ liệu UART",
    "UNO_OFFLINE": "Mất dữ liệu UART từ UNO", "EVENT_LOSS": "Hàng đợi ESP32 đã quá giới hạn",
}


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def connect():
    STORE.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(DB, timeout=15)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA busy_timeout=15000")
    con.execute("""CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        uid TEXT NOT NULL UNIQUE,
        ts TEXT NOT NULL, device TEXT NOT NULL,
        kind TEXT NOT NULL, details TEXT NOT NULL,
        source TEXT NOT NULL, synced INTEGER NOT NULL DEFAULT 0
    )""")
    con.execute("""CREATE TABLE IF NOT EXISTS sensor_samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts TEXT NOT NULL, payload TEXT NOT NULL,
        synced INTEGER NOT NULL DEFAULT 0
    )""")
    con.commit()
    return con


@contextmanager
def database():
    con = connect()
    try:
        with con:
            yield con
    finally:
        con.close()


def number(raw: Any):
    try:
        n = float(raw)
        return n if n == n and abs(n) < 1e7 else None
    except (ValueError, TypeError):
        return None


def flag(raw: Any) -> bool:
    return raw is True or str(raw).lower() in ("1", "true", "on")


def device_for_event(kind: str) -> str:
    if kind.startswith("DOOR") or kind == "ACCESS_DENIED": return "Cửa"
    if kind == "SYSTEM_CMD": return "Điều khiển"
    if kind.startswith("PUMP1") or kind.startswith("MODE") or kind == "WATER_CMD": return "Tưới cây"
    if kind.startswith("PUMP2") or kind.startswith("FLOOD"): return "Chống ngập"
    if kind.startswith("ROOF") or kind.startswith("RAIN"): return "Mái che / Mưa"
    if kind.startswith("CAR"): return "Bãi đỗ xe"
    if kind == "VIBRATION": return "An ninh"
    return "Kết nối"


def collect(raw: dict, esp_http: Callable):
    """Called by eco_demo.polling_task ONLY when LIVE ESP32 returned /data.
    Fetch event journal, commit SQLite first, then ACK ESP32. Retries are deduplicated.
    """
    global LAST_SAMPLE
    uno_available = flag(raw.get("unoConnected"))
    with LOCK:
        now = time.monotonic()
        if now - LAST_SAMPLE >= 60:
            sample = {
                "temperature": number(raw.get("temperature")) if uno_available else None,
                "humidity": number(raw.get("humidity")) if uno_available else None,
                "soil": number(raw.get("soil")) if uno_available else None,
                "light": number(raw.get("light")) if uno_available else None,
                "rain": number(raw.get("rain")) if uno_available else None, "floodRaw": number(raw.get("floodRaw")),
                "distance": number(raw.get("distance")), "vibration": flag(raw.get("vibration")) if uno_available else None,
                "pump1": flag(raw.get("waterPump")) if uno_available else None,
                "pump2": flag(raw.get("floodPump")) if uno_available and flag(raw.get("roofAngleReported")) else None,
                "roofAngle": number(raw.get("roofAngle")) if uno_available and flag(raw.get("roofAngleReported")) else None,
                "roofTargetAngle": number(raw.get("roofTargetAngle")) if uno_available and flag(raw.get("roofAngleReported")) else None,
                "roofMode": ("AUTO" if flag(raw.get("roofAutoMode")) else "MANUAL") if flag(raw.get("manualModesReported")) and uno_available else None,
                "floodMode": ("AUTO" if flag(raw.get("floodAutoMode")) else "MANUAL") if flag(raw.get("manualModesReported")) and uno_available else None,
            }
            with database() as con:
                con.execute("INSERT INTO sensor_samples(ts,payload) VALUES (?,?)", (now_iso(), json.dumps(sample, ensure_ascii=False)))
            LAST_SAMPLE = now

    # The ESP32 /events endpoint needs the enhanced ESP32 firmware.
    # On previous firmware the backend still records periodic sensor samples.
    try:
        packet = json.loads(esp_http('/events'))
        rows = packet.get('events', [])
        if not isinstance(rows, list): return
        rows = rows[:40]
        if not rows:
            maybe_sync()
            return
        boot_key = str(packet.get('bootKey',''))
        esp_uptime = number(packet.get('uptimeMs'))
        received = datetime.now().astimezone()
        with LOCK:
            with database() as con:
                for ev in rows:
                    uid = str(ev.get('id', ''))[:90]
                    kind = str(ev.get('kind', ''))[:40]
                    if not uid or not kind: continue
                    detail = str(ev.get('detail', ''))[:180]
                    source = str(ev.get('source', 'ESP32'))[:30]
                    label = EVENT_LABELS.get(kind, kind)
                    description = f'{label}: {detail}' if detail else label
                    event_time = received
                    event_tick = number(ev.get('uptimeMs'))
                    # Device timestamps are relative to boot. Reliable only for same boot.
                    if (boot_key and uid.startswith(boot_key+'-') and
                        esp_uptime is not None and event_tick is not None):
                        delay_ms = (int(esp_uptime) - int(event_tick)) & 0xFFFFFFFF
                        if delay_ms <= 48*3600*1000:
                            event_time = received-timedelta(milliseconds=delay_ms)
                    con.execute("""INSERT OR IGNORE INTO events(uid,ts,device,kind,details,source)
                                   VALUES (?,?,?,?,?,?)""",
                                (uid, event_time.isoformat(timespec="seconds"), device_for_event(kind), kind, description, source))
        # ACK only after durable SQLite COMMIT; a lost ACK is harmless (unique uid).
        ack = str(rows[-1].get('id', ''))
        if ack:
            esp_http('/events/ack?last='+quote(ack, safe=''), 'POST')
    except Exception:
        # A prior ESP32 firmware has no event journal, or the network may be disrupted.
        # We intentionally keep local sample storage and do not mark LIVE telemetry offline.
        pass
    maybe_sync()


def config():
    defaults = {"enabled": False, "spreadsheet_id": "", "service_account_file": "",
                "events_tab": "NhatKyHoatDong", "sensors_tab": "LichSuCamBien"}
    if CONFIG_FILE.is_file():
        try:
            cfg = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            if isinstance(cfg, dict): defaults.update(cfg)
        except Exception:
            pass
    return defaults


def sync_pending():
    """Đồng bộ SQLite lên Google Sheets, kiểm tra tiêu đề và tránh ghi trùng.

    Luôn giữ dữ liệu trong SQLite nếu Google Sheets không phản hồi. Mỗi lần
    đồng bộ so khớp mã sự kiện/mã mẫu trước khi thêm các dòng còn thiếu.
    """
    global LAST_SYNC_ERROR
    cfg = config()
    if not cfg.get("enabled"):
        LAST_SYNC_ERROR = None
        return {"enabled": False, "uploaded": 0}

    key_file = str(cfg.get("service_account_file") or "").strip()
    key_path = Path(key_file) if key_file else Path("_no_google_key_configured_")
    if not key_path.is_absolute():
        key_path = BASE / key_path
    sheet_id = str(cfg.get("spreadsheet_id") or "").strip()
    if not sheet_id or not key_path.is_file():
        raise RuntimeError("Thiếu spreadsheet_id hoặc tệp service_account.json")

    from google.oauth2.service_account import Credentials
    from google.auth.transport.requests import AuthorizedSession

    creds = Credentials.from_service_account_file(
        str(key_path), scopes=["https://www.googleapis.com/auth/spreadsheets"])
    session = AuthorizedSession(creds)
    api = "https://sheets.googleapis.com/v4/spreadsheets/" + quote(sheet_id, safe="")

    def send(method: str, url: str, **kwargs) -> dict:
        response = session.request(method, url, timeout=15, **kwargs)
        response.raise_for_status()
        return response.json() if response.content else {}

    def range_url(tab: str, a1: str) -> str:
        safe_tab = str(tab).replace("'", "''")
        return api + "/values/" + quote("'" + safe_tab + "'!" + a1, safe="!")

    events_tab = str(cfg.get("events_tab", "NhatKyHoatDong"))
    sensors_tab = str(cfg.get("sensors_tab", "LichSuCamBien"))
    if not events_tab or not sensors_tab or events_tab == sensors_tab:
        raise ValueError("Tên hai trang tính phải khác nhau và không được trống")

    meta = send("GET", api + "?fields=sheets.properties.title")
    existing_tabs = {s.get("properties", {}).get("title") for s in meta.get("sheets", [])}
    to_create = [tab for tab in (events_tab, sensors_tab) if tab not in existing_tabs]
    if to_create:
        send("POST", api + ":batchUpdate", json={"requests": [
            {"addSheet": {"properties": {"title": tab}}} for tab in to_create]})

    def ensure_header(tab: str, header: list[str]) -> None:
        # Không âm thầm ghi đè cột của file Sheets cũ.
        current = send("GET", range_url(tab, "A1:R2")).get("values", [])
        first = current[0] if current else []
        if first == header:
            return
        if any(cell != "" for cell in first) or len(current) > 1:
            raise RuntimeError(
                f"Trang '{tab}' đang dùng định dạng cũ. "
                "Hãy tạo tab mới hoặc đổi tên tab trong eco_sheets_config.json; "
                "không được trộn hai bộ cột trong cùng một trang tính.")
        send("PUT", range_url(tab, "A1"), params={"valueInputOption": "RAW"},
             json={"values": [header]})

    ensure_header(events_tab, HEADERS_EVENTS)
    ensure_header(sensors_tab, HEADERS_SENSORS)

    # Hai cột mã dùng để tránh thêm lại sự kiện đã gửi nếu máy bị ngắt mạng
    # sau khi Google nhận dữ liệu nhưng trước khi SQLite đánh dấu hoàn tất.
    def remote_ids(tab: str, col: str) -> set[str]:
        rows = send("GET", range_url(tab, f"{col}2:{col}")).get("values", [])
        return {str(row[0]) for row in rows if row and row[0] not in (None, "")}

    tasks = [
        (events_tab, "events", "G", event_sheet_row, "uid", None),
        (sensors_tab, "sensor_samples", "R", sensor_sheet_row, "id", "S-"),
    ]
    uploaded = 0
    for tab, table, col, row_builder, id_field, prefix in tasks:
        with LOCK:
            with database() as con:
                pending = con.execute(
                    f"SELECT * FROM {table} WHERE synced=0 ORDER BY id LIMIT 40"
                ).fetchall()
        if not pending:
            continue

        known = remote_ids(tab, col)
        missing = [r for r in pending if (str(prefix or "") + str(r[id_field])) not in known]
        if missing:
            values = [row_builder(r) for r in missing]
            send("POST", range_url(tab, "A1") + ":append",
                 params={"valueInputOption": "RAW", "insertDataOption": "INSERT_ROWS"},
                 json={"values": values})

        # Đánh dấu cả các dòng đã có trên Sheets sau khi API xác nhận thành công.
        with LOCK:
            with database() as con:
                con.executemany(f"UPDATE {table} SET synced=1 WHERE id=?",
                                [(r["id"],) for r in pending])
        uploaded += len(missing)

    LAST_SYNC_ERROR = None
    return {"enabled": True, "uploaded": uploaded}

def _sync_worker():
    global SYNC_BUSY, LAST_SYNC_ERROR
    try:
        sync_pending()
    except Exception as e:
        LAST_SYNC_ERROR = f"{type(e).__name__}: {e}"[:220]
    finally:
        with LOCK:
            SYNC_BUSY = False


def maybe_sync():
    global LAST_SYNC, SYNC_BUSY
    if not config().get('enabled'): return
    with LOCK:
        if SYNC_BUSY or time.monotonic() - LAST_SYNC < 30: return
        SYNC_BUSY = True
        LAST_SYNC = time.monotonic()
    threading.Thread(target=_sync_worker, daemon=True, name="garden-sheets-sync").start()


@router.get("/api/eco/history")
def history(limit: int = Query(80, ge=1, le=500), device: str = ""):
    with LOCK:
        with database() as con:
            if device:
                rows = con.execute("SELECT * FROM events WHERE device=? ORDER BY id DESC LIMIT ?",(device,limit)).fetchall()
            else:
                rows = con.execute("SELECT * FROM events ORDER BY id DESC LIMIT ?",(limit,)).fetchall()
    return {"events":[dict(r) for r in rows]}


@router.get("/api/eco/history/sensors")
def samples(limit: int = Query(100,ge=1,le=500)):
    with LOCK:
        with database() as con:
            rows = con.execute("SELECT * FROM sensor_samples ORDER BY id DESC LIMIT ?",(limit,)).fetchall()
    return {"samples":[{"id":r['id'],"timestamp":r['ts'],**json.loads(r['payload'])} for r in rows]}


@router.get("/api/eco/history/status")
def status():
    with LOCK:
        with database() as con:
            total=con.execute("SELECT count(*) FROM events").fetchone()[0]
            pending=con.execute("SELECT count(*) FROM events WHERE synced=0").fetchone()[0]
            sensor_pending=con.execute("SELECT count(*) FROM sensor_samples WHERE synced=0").fetchone()[0]
    cfg=config()
    return {"events":total,"pending_events":pending,"pending_sensors":sensor_pending,
            "sheets_configured":bool(cfg.get('enabled') and cfg.get('spreadsheet_id')),
            "sync_error":LAST_SYNC_ERROR, "database":"garden_data/eco_history.sqlite3"}


def csv_guard(value):
    s = "" if value is None else str(value)
    return "'"+s if s.startswith(("=", "+", "-", "@", "\t", "\r")) else s


@router.get("/api/eco/history/export.csv")
def export_csv():
    """Xuất CSV theo đúng thứ tự cột Nhật ký Google Sheets."""
    with LOCK:
        with database() as con:
            rows = con.execute("SELECT * FROM events ORDER BY id DESC LIMIT 20000").fetchall()
    out = io.StringIO()
    writer = csv.writer(out)
    writer.writerow(HEADERS_EVENTS)
    for row in rows:
        writer.writerow([csv_guard(v) for v in event_sheet_row(row)])
    content = ("\ufeff" + out.getvalue()).encode("utf-8")
    return StreamingResponse(io.BytesIO(content), media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": "attachment; filename=SmartEcoGarden_LichSu.csv"})


@router.get("/api/eco/history/sensors/export.csv")
def export_sensors_csv():
    """Download recorded LIVE sensor samples; separate from event history."""
    with LOCK:
        with database() as con:
            rows = con.execute("SELECT * FROM sensor_samples ORDER BY id DESC LIMIT 20000").fetchall()
    out = io.StringIO()
    writer = csv.writer(out)
    writer.writerow(HEADERS_SENSORS)
    for row in rows:
        writer.writerow([csv_guard(value) for value in sensor_sheet_row(row)])
    content = ("\ufeff" + out.getvalue()).encode("utf-8")
    return StreamingResponse(io.BytesIO(content), media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition":"attachment; filename=SmartEcoGarden_CamBien.csv"})
