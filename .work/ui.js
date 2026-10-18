
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
