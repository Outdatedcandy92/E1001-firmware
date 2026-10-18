#pragma once
// Served at GET /. Converts images to 1-bit in the browser (hard threshold by default,
// dithering only if asked), then POSTs the raw 48000-byte frame to /image.
static const char WEB_UI[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>reTerminal</title>
<style>
:root{--bg:#f4f4f2;--fg:#111;--card:#fff;--mute:#666;--line:#ddd;--acc:#111}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#eee;--card:#1b1b1b;--mute:#999;--line:#333;--acc:#eee}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:880px;margin:auto;padding:16px}h1{font-size:22px;margin:8px 0 16px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
h2{font-size:15px;margin:0 0 10px;text-transform:uppercase;letter-spacing:.05em;color:var(--mute)}
canvas{width:100%;height:auto;border:1px solid var(--line);background:#fff;image-rendering:pixelated}
button,input,select,textarea{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:6px 10px}
button{cursor:pointer;background:var(--acc);color:var(--bg);border:0}button.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0}textarea{width:100%;min-height:70px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:8px}.grid div{font-size:13px}.grid b{display:block;font-size:17px}
label{font-size:13px;color:var(--mute)}input[type=text],input[type=number],input[type=password]{width:100%}
#msg{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--acc);color:var(--bg);padding:8px 14px;border-radius:8px;display:none}
</style></head><body><main>
<h1>reTerminal E1001</h1>
<section><h2>Status</h2><div class="grid" id="st"></div>
<div class="row"><button onclick="act('refresh')">Fetch now</button><button class="ghost" onclick="act('clear')">Clear</button>
<button class="ghost" onclick="act('sleep')">Sleep</button><button class="ghost" onclick="act('reboot')">Reboot</button><button class="ghost" onclick="loadScreen()">Show current screen</button></div></section>
<section><h2>Push image</h2><input type="file" id="file" accept="image/*">
<div class="row"><label>Threshold <input type="range" id="th" min="1" max="254" value="128"></label>
<label><input type="checkbox" id="dither"> Dither</label><label><input type="checkbox" id="inv"> Invert</label>
<select id="fit"><option>contain</option><option>cover</option><option>stretch</option></select>
<select id="mode"><option>auto</option><option>full</option><option>fast</option><option>partial</option></select>
<button onclick="pushCanvas()">Send</button></div>
<canvas id="cv" width="800" height="480"></canvas></section>
<section><h2>Push text</h2><textarea id="txt">Hello from Hack Club</textarea>
<div class="row"><label>Size <select id="tsize"><option>1</option><option selected>2</option><option>3</option><option>4</option></select></label><button onclick="pushText()">Send text</button></div></section>
<section><h2>Config</h2><div class="grid" id="cfg"></div><div class="row"><button onclick="saveCfg()">Save</button></div></section>
<section><h2>Firmware update</h2><div class="row"><input type="file" id="fw" accept=".bin"><button onclick="ota()">Upload</button></div></section>
</main><div id="msg"></div>
<script>
const $=id=>document.getElementById(id),cv=$('cv'),cx=cv.getContext('2d');let src=null;
function toast(t){const m=$('msg');m.textContent=t;m.style.display='block';clearTimeout(m._t);m._t=setTimeout(()=>m.style.display='none',3000)}
async function act(a){toast(a+'…');const r=await fetch('/'+a,{method:'POST'});toast(await r.text());status()}
const FIELDS={url:'text',interval:'number',awake:'number',usb_awake:'number',full_every:'number',host:'text',ssid:'text',pass:'password',ssid2:'text',pass2:'password'};
async function status(){const s=await (await fetch('/status')).json();
 $('st').innerHTML=Object.entries(s).filter(([k])=>k!='config').map(([k,v])=>`<div>${k}<b>${v}</b></div>`).join('');
 if(!$('cfg').children.length)$('cfg').innerHTML=Object.entries(FIELDS).map(([k,t])=>`<div><label>${k}</label><input type="${t}" id="c_${k}" value="${s.config[k]??''}"></div>`).join('')}
async function saveCfg(){const c={};for(const[k,t]of Object.entries(FIELDS)){const v=$('c_'+k).value;if(k.startsWith('pass')&&!v)continue;c[k]=t=='number'?+v:v}
 const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(c)});toast(await r.text())}
$('file').onchange=e=>{const i=new Image();i.onload=()=>{src=i;render()};i.src=URL.createObjectURL(e.target.files[0])};
['th','dither','inv','fit'].forEach(id=>$(id).oninput=render);
function render(){if(!src)return;cx.fillStyle='#fff';cx.fillRect(0,0,800,480);const f=$('fit').value;let w=800,h=480,x=0,y=0;
 if(f!='stretch'){const s=(f=='cover'?Math.max:Math.min)(800/src.width,480/src.height);w=src.width*s;h=src.height*s;x=(800-w)/2;y=(480-h)/2}
 cx.drawImage(src,x,y,w,h);const d=cx.getImageData(0,0,800,480),p=d.data,th=+$('th').value,inv=$('inv').checked,g=new Float32Array(800*480);
 for(let i=0;i<g.length;i++){let v=.299*p[i*4]+.587*p[i*4+1]+.114*p[i*4+2];if(p[i*4+3]<128)v=255;g[i]=inv?255-v:v}
 for(let i=0;i<g.length;i++){const o=g[i]<th?0:255,e=g[i]-o;g[i]=o;if($('dither').checked){const x=i%800;
  if(x<799)g[i+1]+=e*7/16;if(i+800<g.length){if(x>0)g[i+799]+=e*3/16;g[i+800]+=e*5/16;if(x<799)g[i+801]+=e/16}}}
 for(let i=0;i<g.length;i++){p[i*4]=p[i*4+1]=p[i*4+2]=g[i];p[i*4+3]=255}cx.putImageData(d,0,0)}
function packCanvas(){const p=cx.getImageData(0,0,800,480).data,b=new Uint8Array(48000);
 for(let i=0;i<384000;i++)if(p[i*4]<128)b[i>>3]|=0x80>>(i&7);return b}
async function pushCanvas(){toast('sending…');const r=await fetch('/image',{method:'POST',headers:{'X-Mode':$('mode').value},body:packCanvas()});toast(await r.text())}
async function pushText(){toast('sending…');const r=await fetch('/text',{method:'POST',headers:{'X-Size':$('tsize').value,'Content-Type':'application/octet-stream'},body:$('txt').value});toast(await r.text())}
async function loadScreen(){const b=new Uint8Array(await (await fetch('/screen.bin')).arrayBuffer()),d=cx.createImageData(800,480);
 for(let i=0;i<384000;i++){const v=b[i>>3]&(0x80>>(i&7))?0:255;d.data[i*4]=d.data[i*4+1]=d.data[i*4+2]=v;d.data[i*4+3]=255}cx.putImageData(d,0,0)}
async function ota(){const f=$('fw').files[0];if(!f)return;toast('uploading…');const r=await fetch('/ota',{method:'POST',body:f});toast(await r.text())}
status();loadScreen();
</script></body></html>)HTML";
