"""Smart Eco Garden: REST API LIVE/DEMO adapter for ESP32 and Next.js."""
from __future__ import annotations
from app import eco_history
import asyncio
import base64
import json
import os
import time
import urllib.error
import urllib.request
from collections import deque
from pathlib import Path
from threading import Lock
from typing import Literal

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

router = APIRouter()
ROOT = Path(__file__).resolve().parent
ESP_BASE = os.environ.get('ECO_ESP_URL', 'http://192.168.4.1').rstrip('/')
ESP_USER = os.environ.get('ECO_ESP_USER', 'admin')
ESP_PASS = os.environ.get('ECO_ESP_PASS', 'EcoHome2026!')
_lock = Lock()
_mode = 'live'  # LIVE by default: logs start collecting on Backend boot.
_started = time.monotonic()
_last_seen = None
_error = None
_hist = deque(maxlen=120)
_demo = {
    'unoConnected': True, 'doorOpen': False, 'distance': 16.0,
    'floodRaw': 0, 'temperature': 29.5, 'humidity': 70.0,
    'soil': 510, 'light': 290, 'rain': 1020, 'vibration': 0,
    'waterPump': 0, 'waterMode': 0, 'floodPump': False,
    'roofAngle': 180, 'roofTargetAngle': 180, 'roofAutoMode': False, 'floodAutoMode': False,
    '_flood_manual_ready': True, '_flood_pump_until': 0.0,
    '_flood_alarm': False, '_auto_armed': False,
}
_live = {}
_demo_pump_until = 0.0
_demo_door_until = 0.0
_demo_flood_started = 0.0
_demo_flood_low_since = 0.0


def num(x, default=None):
    try:
        f = float(x)
        return f if f == f and abs(f) < 10000000 else default
    except (ValueError, TypeError):
        return default


def boolean(x):
    return x is True or str(x).strip().lower() in ('1','true','on','bat')


def normalize(raw, demo):
    """Canonical Next.js-facing data; prefer real UNO telemetry over guesses."""
    uno = True if demo else boolean(raw.get('unoConnected'))
    f = num(raw.get('floodRaw'), 0 if demo else None)
    soil = num(raw.get('soil')) if uno else None
    rain = num(raw.get('rain')) if uno else None
    distance = num(raw.get('distance'))
    mode = raw.get('waterMode', 0)
    rain_wet = rain is not None and rain < 900
    has_new_uart = boolean(raw.get('roofAngleReported')) if not demo else True
    has_manual_modes = demo or boolean(raw.get('manualModesReported'))
    alarm = (boolean(raw.get('floodAlarm')) if 'floodAlarm' in raw and has_new_uart
             else bool(raw.get('_flood_alarm', False)) if demo
             else f is not None and f >= 700)
    pump2 = (boolean(raw.get('floodPump')) if demo or has_new_uart else None)
    roof_angle = num(raw.get('roofAngle')) if has_new_uart else None
    roof_target = num(raw.get('roofTargetAngle')) if has_new_uart else None
    roof_state = (str(raw.get('roofState')) if not demo else
                  ('MOVING' if roof_angle != roof_target else ('COVERED' if roof_target == 90 else 'RETRACTED'))) if has_new_uart else 'UNKNOWN'
    if roof_state not in ('MOVING','COVERED','RETRACTED','UNKNOWN'): roof_state = 'UNKNOWN'
    return {
        'temperature': num(raw.get('temperature')) if uno else None,
        'humidity': num(raw.get('humidity')) if uno else None,
        'soilRaw': soil, 'lightRaw': num(raw.get('light')) if uno else None,
        'rainRaw': rain, 'raining': rain_wet,
        'vibration': boolean(raw.get('vibration')) if uno else False,
        'floodRaw': f,
        'floodAlert': alarm if (demo or uno) else False,
        'floodStatus': 'Cao' if f is not None and f >= 700 else ('Thap' if f is not None and f <= 100 else 'Trung binh'),
        'pump1': boolean(raw.get('waterPump', raw.get('relayState', 0))) if uno else False,
        'pump2': pump2 if uno else None,
        'waterMode': ('AUTO' if boolean(mode) else 'MANUAL') if uno else 'UNKNOWN',
        'doorOpen': boolean(raw.get('doorOpen')),
        'distanceCm': distance,
        'carDetected': boolean(raw.get('carDetected')) if 'carDetected' in raw else (distance is not None and 2 <= distance <= 5.5),
        'unoConnected': uno,
        'ledRed': bool(alarm) if uno else False,
        'ledGreen': f is not None and not alarm and uno,
        'ledYellow': boolean(raw.get('waterPump')) and uno,
        'soilStatus': 'Kho' if soil is not None and soil >= 850 else ('Am' if soil is not None and soil <= 650 else 'Trung binh' if soil is not None else 'Khong co du lieu'),
        # Servo SG90 does not provide physical position feedback.
        'roofAngleReported': has_new_uart and uno,
        'roofAngle': roof_angle if uno else None,
        'roofTargetAngle': roof_target if uno else None,
        'roofCoverRequested': boolean(raw.get('roofCoverRequested')) if has_new_uart and uno else False,
        'roofState': roof_state if uno else 'UNKNOWN',
        'roofWeather': str(raw.get('roofWeather', 'UNKNOWN')),
        'manualModesReported': has_manual_modes and uno,
        'roofMode': ('AUTO' if boolean(raw.get('roofAutoMode')) else 'MANUAL') if has_manual_modes and uno else 'UNKNOWN',
        'floodMode': ('AUTO' if boolean(raw.get('floodAutoMode')) else 'MANUAL') if has_manual_modes and uno else 'UNKNOWN',
        'eventQueue': int(num(raw.get('eventQueue'), 0)) if not demo else None,
    }

def snapshot():
    with _lock:
        mode = _mode
        raw = dict(_demo if mode == 'demo' else _live)
        last_seen = _last_seen
        error = _error
    fresh = mode == 'demo' or (last_seen is not None and time.monotonic() - last_seen < 6)
    return {'mode': mode, 'simulated': mode == 'demo', 'connected': bool(fresh),
            'error': error if mode == 'live' else None,
            'timestamp': time.time(),
            'data': normalize(raw, mode == 'demo') if raw and fresh else None,
            'history': list(_hist)}


def http_esp(path, method='GET'):
    auth = base64.b64encode((ESP_USER + ':' + ESP_PASS).encode()).decode()
    req = urllib.request.Request(ESP_BASE+path, method=method,
        headers={'Authorization': 'Basic '+auth,'Accept':'application/json'})
    with urllib.request.urlopen(req, timeout=2.5) as rsp:
        payload = rsp.read(50000)
        return json.loads(payload) if path == '/data' else payload.decode('utf-8','replace')


async def polling_task():
    global _live, _last_seen, _error
    while True:
        if _mode == 'live':
            try:
                result = await asyncio.to_thread(http_esp, '/data')
                if not isinstance(result, dict):
                    raise ValueError('ESP32 /data khong tra ve JSON object')
                # SMART_ECO_HISTORY: database errors must not mark ESP32 offline.
                try:
                    await asyncio.to_thread(eco_history.collect, result, http_esp)
                except Exception as log_exc:
                    print('[GARDEN HISTORY] ', log_exc)
                with _lock:
                    _live = result
                    _last_seen = time.monotonic()
                    _error = None
            except Exception as e:
                with _lock:
                    _error = f'{type(e).__name__}: {e}'[:200]
        snap = snapshot()
        if snap['data']:
            with _lock:
                _hist.append({'t': int(time.time()),
                              'temperature': snap['data']['temperature'],
                              'soilRaw': snap['data']['soilRaw'],
                              'floodRaw': snap['data']['floodRaw']})
        await asyncio.sleep(1.5)


@router.get('/api/eco/status')
def api_status():
    return snapshot()


class ModeRequest(BaseModel):
    mode: Literal['demo','live']


@router.post('/api/eco/mode')
def set_mode(body: ModeRequest):
    global _mode, _last_seen, _error
    with _lock:
        _mode = body.mode
        _last_seen = None
        _error = None
        _hist.clear()
    return {'mode': body.mode, 'simulated': body.mode == 'demo'}


class SensorRequest(BaseModel):
    floodRaw: int | None = None
    soil: int | None = None
    rain: int | None = None
    distance: float | None = None
    vibration: bool | None = None


@router.post('/api/eco/demo/sensors')
def demo_sensors(body: SensorRequest):
    if _mode != 'demo':
        raise HTTPException(status_code=409, detail='Chinh cam bien chi dung trong che do mo phong')
    obj = body.model_dump(exclude_none=True)
    for name, val in obj.items():
        if name in ('floodRaw','soil','rain') and not (0 <= val <= 4095):
            raise HTTPException(status_code=422,detail=f'{name} phai tu 0 den 4095')
        if name == 'distance' and not (-1 <= val <= 500):
            raise HTTPException(status_code=422,detail='Khoang cach khong hop le')
    with _lock:
        _demo.update(obj)
        if 'vibration' in obj:
            _demo['vibration'] = 1 if obj['vibration'] else 0
    return snapshot()


class CommandRequest(BaseModel):
    command: Literal['door/open','door/close','water/auto','water/manual','water/on','water/off',
                     'roof/auto','roof/manual','roof/open','roof/close',
                     'flood/auto','flood/manual','flood/on','flood/off']


@router.post('/api/eco/command')
async def command(body: CommandRequest):
    global _demo_door_until, _demo_pump_until
    c = body.command
    if _mode == 'demo':
        with _lock:
            if c == 'door/open':
                _demo['doorOpen'] = True
                _demo_door_until = time.monotonic() + 5
            elif c == 'door/close':
                _demo['doorOpen'] = False
                _demo_door_until = 0
            elif c == 'water/auto':
                _demo['waterMode'] = 1
                _demo['_auto_armed'] = _demo['soil'] <= 650
                _demo['waterPump'] = 0
                _demo_pump_until = 0
            elif c == 'water/manual':
                _demo['waterMode'] = 0
                _demo['_auto_armed'] = False
                _demo['waterPump'] = 0
                _demo_pump_until = 0
            elif c == 'water/on':
                if _demo['waterMode'] != 0 or _demo['rain'] < 900:
                    raise HTTPException(status_code=409,detail='Chi bat bom khi MANUAL va khong co mua')
                _demo['waterPump'] = 1
                _demo_pump_until = time.monotonic() + 5
            elif c == 'water/off':
                _demo['waterPump'] = 0
                _demo_pump_until = 0
            elif c == 'roof/auto':
                _demo['roofAutoMode'] = True
            elif c == 'roof/manual':
                _demo['roofAutoMode'] = False
            elif c in ('roof/open', 'roof/close'):
                if _demo['roofAutoMode']:
                    raise HTTPException(status_code=409, detail='Chuyen mai che sang MANUAL truoc')
                _demo['roofTargetAngle'] = 90 if c.endswith('open') else 180
            elif c == 'flood/auto':
                _demo['floodAutoMode'] = True
                _demo['floodPump'] = False
                _demo['_flood_lockout'] = True
                _demo['_flood_manual_ready'] = True
            elif c == 'flood/manual':
                _demo['floodAutoMode'] = False
                _demo['floodPump'] = False
                _demo['_flood_manual_ready'] = True
            elif c == 'flood/on':
                if _demo['floodAutoMode'] or not _demo['_flood_manual_ready'] or _demo['floodRaw'] <= 100:
                    raise HTTPException(status_code=409, detail='Chuyen MANUAL va dam bao co nuoc')
                _demo['floodPump'] = True
                _demo['_flood_manual_ready'] = False
                _demo['_flood_pump_until'] = time.monotonic() + 30
            elif c == 'flood/off':
                if _demo['floodAutoMode']:
                    raise HTTPException(status_code=409, detail='Chuyen MANUAL truoc')
                _demo['floodPump'] = False
                _demo['_flood_manual_ready'] = True
        return {'ok':True,'simulated':True,'command':c,'note':'Lenh chi thay doi mo phong, khong dieu khien phan cung'}
    if not snapshot()['connected']:
        raise HTTPException(status_code=503, detail='ESP32 mat ket noi. Khong gui lenh')
    if (c.startswith(('water/','roof/','flood/')) and not boolean(_live.get('unoConnected'))):
        raise HTTPException(status_code=503, detail='UNO mat UART: chan lenh tuoi cay')
    if c.startswith(('roof/','flood/')) and not boolean(_live.get('manualModesReported')):
        raise HTTPException(status_code=409, detail='Chua nap code UNO/ESP32 ho tro AUTO/MANUAL moi')
    if c in ('roof/open','roof/close') and boolean(_live.get('roofAutoMode')):
        raise HTTPException(status_code=409, detail='Mai dang AUTO; chuyen sang MANUAL truoc')
    if c in ('flood/on','flood/off') and boolean(_live.get('floodAutoMode')):
        raise HTTPException(status_code=409, detail='Chong ngap dang AUTO; chuyen MANUAL truoc')
    if c == 'flood/on' and num(_live.get('floodRaw'), 0) <= 100:
        raise HTTPException(status_code=409, detail='Muc nuoc qua thap; khong bat bom de tranh chay kho')
    try:
        # Firmware variants seen in project use GET or POST. POST first for provided .ino.
        try:
            result = await asyncio.to_thread(http_esp, '/'+c, 'POST')
        except urllib.error.HTTPError as ex:
            if c.startswith(('roof/','flood/')) or ex.code not in (404,405): raise
            result = await asyncio.to_thread(http_esp, '/'+c, 'GET')
    except Exception as ex:
        raise HTTPException(status_code=502,detail=f'ESP32 khong nhan lenh: {ex}')
    return {'ok':True,'simulated':False,'command':c,'response':str(result)[:200],
            'note':'ESP32 da phan hoi HTTP; xem /data de xac nhan trang thai thuc'}


async def demo_tick_task():
    global _demo_pump_until, _demo_door_until, _demo_flood_started, _demo_flood_low_since
    while True:
        if _mode == 'demo':
            with _lock:
                t = time.monotonic()
                if _demo_door_until and t >= _demo_door_until:
                    _demo['doorOpen'] = False
                    _demo_door_until = 0
                if _demo_pump_until and t >= _demo_pump_until:
                    _demo['waterPump'] = 0
                    _demo_pump_until = 0
                # Irrigation uses the same arming and rain rules as UNO.
                if _demo['rain'] < 900 and _demo['waterPump']:
                    _demo['waterPump'] = 0
                    _demo_pump_until = 0
                if _demo['waterMode'] == 1:
                    if _demo['soil'] <= 650:
                        _demo['_auto_armed'] = True
                        if _demo['waterPump']:
                            _demo['waterPump'] = 0
                            _demo_pump_until = 0
                    if (_demo['_auto_armed'] and not _demo['waterPump']
                            and _demo['soil'] >= 850 and _demo['rain'] >= 900):
                        _demo['waterPump'] = 1
                        _demo_pump_until = t + 5
                        _demo['_auto_armed'] = False
                # Servo simulated gradual travel; same non-blocking behaviour.
                roof_target = _demo['roofTargetAngle']
                if _demo['roofAutoMode']:
                    # Demo confirms rain quickly; real firmware debounces the sensor.
                    roof_target = 90 if _demo['rain'] < 900 else 180
                    _demo['roofTargetAngle'] = roof_target
                if _demo['roofAngle'] < roof_target:
                    _demo['roofAngle'] = min(roof_target, _demo['roofAngle'] + 5)
                elif _demo['roofAngle'] > roof_target:
                    _demo['roofAngle'] = max(roof_target, _demo['roofAngle'] - 5)
                f = _demo['floodRaw']
                if f >= 700:
                    _demo['_flood_alarm'] = True
                    _demo_flood_low_since = 0
                    if not _demo_flood_started: _demo_flood_started = t
                    if (_demo['floodAutoMode'] and t - _demo_flood_started >= 3
                            and not _demo.get('_flood_lockout', False)):
                        _demo['floodPump'] = True
                        _demo['_flood_lockout'] = True
                        _demo['_flood_pump_until'] = t+30
                elif f <= 100:
                    _demo_flood_started = 0
                    if not _demo_flood_low_since: _demo_flood_low_since = t
                    if not _demo['floodAutoMode']:
                        _demo['floodPump'] = False
                    if t - _demo_flood_low_since >= 3:
                        _demo['floodPump'] = False
                        _demo['_flood_lockout'] = False
                        _demo['_flood_alarm'] = False
                else:
                    _demo_flood_started = 0
                    _demo_flood_low_since = 0
                if _demo.get('floodPump') and t >= _demo.get('_flood_pump_until',t+1):
                    _demo['floodPump'] = False
                    _demo['_flood_manual_ready'] = False
        await asyncio.sleep(.25)


# eco_demo_history_router_marker
router.include_router(eco_history.router)
