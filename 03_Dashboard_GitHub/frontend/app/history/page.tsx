"use client";
import {useCallback, useEffect, useState} from "react";
import Link from "next/link";
import {Activity, ArrowLeft, Download, RefreshCcw, Wifi, CloudOff} from "lucide-react";

type Item={id:number;uid:string;ts:string;device:string;kind:string;details:string;source:string;synced:number};
type Overview={events:number;pending_events:number;pending_sensors:number;sheets_configured:boolean;sync_error:string|null};
type Sample={id:number;timestamp:string;temperature:number|null;humidity:number|null;soil:number|null;light:number|null;rain:number|null;floodRaw:number|null;distance:number|null;roofAngle:number|null;roofTargetAngle:number|null;};
const groups=["Tất cả","Cửa","Tưới cây","Mái che / Mưa","Chống ngập","Bãi đỗ xe","An ninh","Kết nối"];

export default function HistoryPage(){
 const [items,setItems]=useState<Item[]>([]);
 const [samples,setSamples]=useState<Sample[]>([]);
 const [info,setInfo]=useState<Overview|null>(null);
 const [device,setDevice]=useState("Tất cả");
 const [error,setError]=useState("");
 const [busy,setBusy]=useState(false);
 const load=useCallback(async()=>{
  try {
   const q=device==="Tất cả"?"":"&device="+encodeURIComponent(device);
   const [a,b,c]=await Promise.all([
    fetch("/api/eco/history?limit=150"+q,{cache:"no-store"}),
    fetch("/api/eco/history/status",{cache:"no-store"}),
    fetch("/api/eco/history/sensors?limit=20",{cache:"no-store"}),
   ]);
   if(!a.ok||!b.ok||!c.ok)throw Error("Backend không trả về dữ liệu lịch sử");
   const [ev,stat,sam]=await Promise.all([a.json(),b.json(),c.json()]);
   setItems(ev.events||[]);setInfo(stat);setSamples(sam.samples||[]);setError("");
  } catch(e){setError(e instanceof Error?e.message:"Không thể truy cập Backend");}
 },[device]);
 useEffect(()=>{void load();const timer=setInterval(()=>void load(),4000);return()=>clearInterval(timer)},[load]);
 const num=(v:number|null|undefined)=>v===null||v===undefined?"--":String(v);
 return <div className="min-h-screen bg-[#f5f7f4] text-stone-900">
 <header className="border-b border-emerald-900/10 bg-white">
  <div className="mx-auto flex max-w-6xl flex-wrap items-center justify-between gap-3 px-4 py-5 sm:px-6">
   <div><div className="flex items-center gap-3"><Activity className="text-emerald-700"/><h1 className="text-xl font-bold">Nhật ký Smart Eco Garden</h1></div><p className="mt-1 text-xs text-stone-500">Hoạt động của các thiết bị được lưu tự động theo thời gian</p></div>
   <div className="flex gap-2"><Link href="/" className="flex items-center gap-2 rounded-xl border bg-white px-3 py-2 text-sm"><ArrowLeft className="h-4 w-4"/>Tổng quan</Link><a href="/api/eco/history/export.csv" className="flex items-center gap-2 rounded-xl bg-emerald-700 px-3 py-2 text-sm text-white"><Download className="h-4 w-4"/>Xuất sự kiện</a></div>
  </div>
 </header>
 <main className="mx-auto max-w-6xl space-y-5 px-4 py-6 sm:px-6">
 <div className="grid gap-3 sm:grid-cols-3">
  <div className="rounded-2xl border bg-white p-4"><p className="text-xs text-stone-500">Tổng sự kiện đã lưu</p><p className="mt-2 text-3xl font-bold">{info?.events ?? "--"}</p></div>
  <div className="rounded-2xl border bg-white p-4"><p className="text-xs text-stone-500">Sự kiện chờ Sheets</p><p className="mt-2 text-3xl font-bold">{info?.pending_events ?? "--"}</p></div>
  <div className="rounded-2xl border bg-white p-4"><p className="text-xs text-stone-500">Đồng bộ Google Sheets</p><div className="mt-3 flex items-center gap-2 text-sm font-semibold">{info?.sheets_configured?<><Wifi className="h-4 w-4 text-emerald-600"/>Đã cấu hình</>:<><CloudOff className="h-4 w-4 text-stone-400"/>Chưa cấu hình</>}</div></div>
 </div>
 {info?.sync_error && <p className="rounded-xl border border-amber-200 bg-amber-50 p-3 text-xs text-amber-800">Đồng bộ Sheets gặp lỗi: {info.sync_error}. SQLite vẫn lưu dữ liệu cục bộ.</p>}
 <section className="rounded-2xl border bg-white p-4 shadow-sm sm:p-5">
  <div className="flex flex-wrap items-center justify-between gap-3"><h2 className="text-base font-bold">Nhật ký hoạt động</h2><div className="flex gap-2"><select aria-label="Lọc thiết bị" className="rounded-lg border p-2 text-sm" value={device} onChange={e=>setDevice(e.target.value)}>{groups.map(g=><option key={g} value={g}>{g}</option>)}</select><button type="button" onClick={async()=>{setBusy(true);await load();setBusy(false)}} disabled={busy} className="rounded-lg border p-2"><RefreshCcw className="h-4 w-4"/></button></div></div>
  {error&&<p className="mt-3 rounded-lg bg-rose-50 p-3 text-sm text-rose-700">{error}</p>}
  <div className="mt-4 overflow-x-auto">
    {items.length === 0 ? (
      <p className="rounded-lg bg-stone-50 p-5 text-sm text-stone-500">Chưa có sự kiện được ghi nhận.</p>
    ) : (
      <table className="w-full min-w-[980px] border-collapse text-left text-xs">
        <thead className="border-b bg-stone-50 text-stone-700">
          <tr>{["TimeStamp", "Time", "Date", "Thiết bị", "Hoạt động", "Nguồn", "Đồng bộ"].map(title => (
            <th key={title} className="px-3 py-3 font-semibold">{title}</th>
          ))}</tr>
        </thead>
        <tbody>
          {items.map(item => {
            const date = new Date(item.ts);
            const valid = Number.isFinite(date.getTime());
            const day = valid ? date.toLocaleDateString("vi-VN", {day:"2-digit",month:"2-digit",year:"numeric"}) : "--";
            const clock = valid ? date.toLocaleTimeString("vi-VN", {hour12:false}) : "--";
            return (
              <tr key={item.uid} className="border-b border-stone-100 align-top">
                <td className="whitespace-nowrap px-3 py-3">{day} {clock}</td>
                <td className="whitespace-nowrap px-3 py-3">{clock}</td>
                <td className="whitespace-nowrap px-3 py-3">{day}</td>
                <td className="px-3 py-3">{item.device}</td>
                <td className="min-w-[230px] px-3 py-3">{item.details}</td>
                <td className="px-3 py-3">{item.source}</td>
                <td className="whitespace-nowrap px-3 py-3 text-stone-500">{item.synced ? "Đã gửi Sheets" : "Đã lưu cục bộ"}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    )}
  </div>
  <p className="mt-3 text-xs text-stone-500">Lịch sử ghi theo thời gian máy tính; sự kiện được lưu cục bộ trước khi đồng bộ lên Google Sheets.</p>
 </section>
 <section className="rounded-2xl border bg-white p-4 shadow-sm sm:p-5"><div className="flex flex-wrap items-center justify-between gap-2"><h2 className="text-base font-bold">20 mẫu cảm biến gần nhất</h2><a href="/api/eco/history/sensors/export.csv" className="rounded-lg bg-emerald-700 px-3 py-2 text-xs text-white">Xuất cảm biến CSV</a></div><p className="mt-1 text-xs text-stone-500">Dữ liệu môi trường được ghi định kỳ mỗi phút khi kết nối thiết bị thật.</p><div className="mt-4 overflow-x-auto"><table className="w-full min-w-[850px] text-left text-xs"><thead className="bg-emerald-50 text-emerald-900"><tr>{["Thời gian","Nhiệt độ","Đất RAW","Mưa RAW","Ngập RAW","Góc mái lệnh","Mục tiêu"].map(t=><th className="p-2" key={t}>{t}</th>)}</tr></thead><tbody>{samples.map(s=><tr key={s.id} className="border-b border-stone-100"><td className="p-2">{s.timestamp.replace("T"," ").slice(0,19)}</td><td className="p-2">{num(s.temperature)}</td><td className="p-2">{num(s.soil)}</td><td className="p-2">{num(s.rain)}</td><td className="p-2">{num(s.floodRaw)}</td><td className="p-2">{num(s.roofAngle)}°</td><td className="p-2">{num(s.roofTargetAngle)}°</td></tr>)}</tbody></table></div></section>
 <p className="pb-4 text-xs text-stone-500">Thời gian do máy tính ghi nhận. Góc mái hiển thị là góc điều khiển, chưa có cảm biến xác nhận vị trí thực tế.</p>
 </main></div>;
}
