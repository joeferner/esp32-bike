// Web UI served at /. Raw string literal, so edit as plain HTML/CSS/JS.
#pragma once
#include <Arduino.h>

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bike</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32' fill='none' stroke='%232563eb' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><circle cx='7' cy='21' r='5.5'/><circle cx='25' cy='21' r='5.5'/><path d='M7 21h8l-4-9h10M15 21l6-9 4 9M9 9h5M19 8h3l-1 4'/></svg>">
<style>
:root{--bg:#f4f4f5;--card:#fff;--fg:#18181b;--mute:#71717a;--accent:#2563eb;--low:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#09090b;--card:#18181b;--fg:#fafafa;--mute:#a1a1aa;--accent:#60a5fa;--low:#f87171}}
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:0 16px 16px}
header{display:flex;justify-content:space-between;align-items:center;height:36px;color:var(--mute);font-size:13px}
.status{display:flex;align-items:center;gap:6px}
#wifi path{fill:none;stroke:currentColor;stroke-width:2.2;stroke-linecap:round}
#wifi circle{fill:currentColor}
#wifi .off{opacity:.25}
#batt{fill:currentColor}
#batt.low{color:var(--low)}
main{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:12px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
.tile{background:var(--card);border-radius:14px;padding:14px 16px}
.v{font-size:2em;font-weight:700;font-variant-numeric:tabular-nums}
.row .v{font-size:3.4em;line-height:1.1}
.l{color:var(--mute);font-size:13px}
.s{color:var(--mute);font-size:13px;margin-top:2px;font-variant-numeric:tabular-nums}
canvas{display:block;width:100%;height:140px;margin-top:6px}
.actions{display:flex;gap:12px}
#volts{margin-left:2px;font-variant-numeric:tabular-nums}
#sleepin{color:var(--low);margin-right:4px;font-variant-numeric:tabular-nums}
#asleep{text-align:center;padding:20px 16px;border:1px solid var(--accent)}
#asleep .v{font-size:1.4em}
[hidden]{display:none!important}
button{flex:1;font:inherit;padding:10px;border-radius:10px;border:1px solid var(--mute);background:transparent;color:var(--fg)}
</style></head><body>
<header><span>Bike</span><span class="status">
<span id="sleepin" hidden></span>
<span id="rssi"></span>
<svg id="wifi" viewBox="0 0 24 18" width="20" height="15"><circle cx="12" cy="15.5" r="1.8"/>
<path d="M8.46 12.46A5 5 0 0 1 15.54 12.46"/><path d="M5.64 9.64A9 9 0 0 1 18.36 9.64"/><path d="M2.81 6.81A13 13 0 0 1 21.19 6.81"/></svg>
<svg id="batt" viewBox="0 0 27 13" width="27" height="13"><rect x=".75" y=".75" width="22.5" height="11.5" rx="3" fill="none" stroke="currentColor" stroke-width="1.5"/>
<rect x="24.5" y="4" width="2" height="5" rx="1"/><rect id="bfill" x="2.5" y="2.5" height="8" rx="1.5" width="0"/></svg>
<span id="bpct"></span><span id="volts"></span></span></header>
<main>
<div class="tile" id="asleep" hidden><div class="v">Bike is asleep</div><div class="l">Pedal to wake it up</div></div>
<div class="row">
 <div class="tile"><div class="v" id="spd">-</div><div class="l">Speed (mph)</div><div class="s" id="avgspd"></div></div>
 <div class="tile"><div class="v" id="rpm">-</div><div class="l">RPM</div></div>
</div>
<div class="tile"><div class="l">RPM, last 5 min</div><canvas id="g"></canvas></div>
<div class="grid">
 <div class="tile"><div class="v" id="avg">-</div><div class="l">Average RPM</div></div>
 <div class="tile"><div class="v" id="max">-</div><div class="l">Max RPM</div><div class="s" id="maxspd"></div></div>
 <div class="tile"><div class="v" id="dist">-</div><div class="l">Distance (mi)</div></div>
 <div class="tile"><div class="v" id="time">-</div><div class="l">Time ridden</div></div>
 <div class="tile"><div class="v" id="revs">-</div><div class="l">Revolutions</div></div>
</div>
<div class="actions"><button id="reset">Reset ride</button></div>
</main>
<script>
const C=2.105, N=300; // wheel circumference in m (700x25c), graph samples
const MI=1609.344; // m per mile
const GEAR=1.65; // virtual gear: wheel revs per crank rev (~9.5 mph at 73 RPM)
const $=id=>document.getElementById(id);
let hist=[], lastTick=-1, last=null, asleep=false;

const speed=r=>r*GEAR*C*60/MI; // mph
const fmtTime=s=>{const h=Math.floor(s/3600),m=Math.floor(s/60)%60,x=s%60;
  return (h?h+':':'')+String(m).padStart(h?2:1,'0')+':'+String(x).padStart(2,'0')};

function render(){
  const d=last; if(!d)return;
  $('spd').textContent=speed(d.rpm).toFixed(1);
  $('avgspd').textContent='avg '+speed(d.avgRpm).toFixed(1)+' mph';
  $('rpm').textContent=d.rpm.toFixed(0);
  $('avg').textContent=d.avgRpm.toFixed(0);
  $('max').textContent=d.maxRpm.toFixed(0);
  $('maxspd').textContent=speed(d.maxRpm).toFixed(1)+' mph';
  $('dist').textContent=(d.revs*GEAR*C/MI).toFixed(2);
  $('time').textContent=fmtTime(d.rideSec);
  document.title=fmtTime(d.rideSec)+(d.rpm?'':' (paused)')+' · Bike';
  $('revs').textContent=d.revs.toLocaleString();
  setStatus(d);
  draw();
}

function setStatus(d){
  const bars=!d?0:d.rssi>-55?4:d.rssi>-65?3:d.rssi>-75?2:d.rssi>-85?1:0;
  [...$('wifi').children].forEach((e,i)=>e.classList.toggle('off',i>=bars));
  $('rssi').textContent=d?d.rssi+' dBm':asleep?'asleep':'offline';
  $('asleep').hidden=!asleep;
  $('sleepin').hidden=!d||asleep||d.sleepSec>=60;
  if(!d)return;
  $('sleepin').textContent='Sleeping in '+d.sleepSec+'s';
  $('bfill').setAttribute('width',d.onUsb?18:18*d.batPct/100);
  $('batt').classList.toggle('low',!d.onUsb&&d.batPct<20);
  $('bpct').textContent=d.onUsb?'USB':d.batPct+'%';
  $('volts').textContent=d.vbat.toFixed(2)+'V';
}

function draw(){
  const cv=$('g'), dpr=devicePixelRatio||1, w=cv.clientWidth, h=cv.clientHeight;
  cv.width=w*dpr; cv.height=h*dpr;
  const x=cv.getContext('2d'); x.scale(dpr,dpr);
  const cs=getComputedStyle(document.documentElement);
  const accent=cs.getPropertyValue('--accent').trim(), mute=cs.getPropertyValue('--mute').trim();
  const top=Math.max(60,Math.ceil(Math.max(0,...hist)*1.15/10)*10);
  x.strokeStyle=mute; x.globalAlpha=.3; x.beginPath(); x.moveTo(0,h/2+.5); x.lineTo(w,h/2+.5); x.stroke(); x.globalAlpha=1;
  x.fillStyle=mute; x.font='11px system-ui'; x.fillText(top,2,11); x.fillText(top/2,2,h/2-4);
  if(hist.length<2)return;
  const step=w/(N-1), off=N-hist.length;
  x.beginPath();
  hist.forEach((v,i)=>{const px=(off+i)*step, py=h-1-v/top*(h-2); i?x.lineTo(px,py):x.moveTo(px,py)});
  x.strokeStyle=accent; x.lineWidth=2; x.lineJoin='round'; x.stroke();
  x.lineTo(w,h); x.lineTo(off*step,h); x.closePath();
  x.globalAlpha=.15; x.fillStyle=accent; x.fill(); x.globalAlpha=1;
}

async function loadHist(){
  const h=await (await fetch('/history')).json();
  hist=h.rpm; lastTick=h.tick;
}

async function poll(){
  try{
    // Time out quickly: a sleeping board never answers, and the default timeout is minutes
    const d=await (await fetch('/data',{signal:AbortSignal.timeout(3000)})).json();
    if(d.tick===lastTick+1){hist.push(Math.round(d.rpm)); if(hist.length>N)hist.shift(); lastTick=d.tick}
    else if(d.tick!==lastTick) await loadHist();
    asleep=d.sleepSec<=1; last=d; render();
  }catch(e){
    // Gone right when the countdown ran out: it went to sleep rather than dropping off WiFi
    if(last&&last.sleepSec<=5)asleep=true;
    setStatus(null); document.title=(asleep?'Asleep':'Offline')+' · Bike';
  }
  setTimeout(poll,1000);
}

$('reset').onclick=async()=>{if(confirm('Reset ride stats?')){await fetch('/reset',{method:'POST'}); await loadHist(); render()}};
addEventListener('resize',draw);
render(); poll();
</script></body></html>)HTML";
