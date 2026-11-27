// The web UI page (served at /). Plain HTML + JS, no external files.
#include <stddef.h>
#include "webPage.h"

const char gWebPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DSPicoDrive</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;margin:0;background:#101214;color:#e8e8e8}
header{padding:12px 16px;background:#1b1f24;border-bottom:1px solid #2c3238}
h1{font-size:1.15em;margin:0}
main{max-width:720px;margin:0 auto;padding:12px}
.card{background:#181b1f;border:1px solid #2c3238;border-radius:10px;padding:12px;margin:12px 0}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.grow{flex:1;min-width:0}
.name{font-weight:600;overflow-wrap:anywhere}
.dim{color:#9aa3ab;font-size:.9em}
.ok{color:#5fd38a}.warn{color:#e8c25a}.bad{color:#ff7a7a}
button{font:inherit;padding:8px 12px;border-radius:8px;border:1px solid #3a424a;background:#232930;color:#e8e8e8}
button.primary{background:#2d6cdf;border-color:#2d6cdf;color:#fff}
button.danger{border-color:#7a2f2f;color:#ff9a9a}
button:disabled{opacity:.45}
input,select{font:inherit;padding:8px;border-radius:8px;border:1px solid #3a424a;background:#101214;color:#e8e8e8}
.img{border-top:1px solid #2c3238;padding:10px 0}
.img:first-child{border-top:0}
#toast{position:fixed;left:12px;right:12px;bottom:12px;max-width:696px;margin:0 auto;padding:12px;border-radius:10px;background:#2a3038;display:none}
</style></head><body>
<header><h1>DSPicoDrive <small>v0.1</small></h1></header>
<main>
<div id="login" class="card" style="display:none">
  <p>Enter the PIN shown on the DS top screen.</p>
  <div class="row"><input id="pin" inputmode="numeric" maxlength="4" placeholder="PIN" style="width:7em">
  <button class="primary" onclick="login()">Sign in</button></div>
</div>
<div id="app" style="display:none">
  <div class="card" id="status">Loading...</div>
  <div class="card">
    <div class="row"><div class="grow"><b>Images</b> <span class="dim">sd:/images</span></div>
    <button onclick="act('/api/rescan')">Rescan</button></div>
    <div id="images"></div>
  </div>
  <div class="card">
    <b>New image</b>
    <div class="row" style="margin-top:8px">
      <input id="newname" class="grow" placeholder="IMAGE_10" maxlength="40">
      <select id="newsize">
        <option value="8">8 MB (Xbox memory unit)</option><option value="32">32 MB</option>
        <option value="128">128 MB</option><option value="512">512 MB</option><option value="1024">1 GB</option>
        <option value="2048">2 GB</option><option value="4095">4095 MB</option><option value="8192">8 GB</option>
        <option value="16384">16 GB</option><option value="32768">32 GB</option><option value="65536">64 GB</option>
      </select>
      <button onclick="createImg()">Create</button>
    </div>
    <div class="dim" style="margin-top:6px">Quick create: blank and ready to format on the PC or Xbox.</div>
  </div>
</div>
<div id="toast"></div>
</main>
<script>
let busy=false,st=null;
const $=id=>document.getElementById(id);
function toast(m,bad){const t=$('toast');t.textContent=m;t.className=bad?'bad':'';t.style.display='block';clearTimeout(t._h);t._h=setTimeout(()=>t.style.display='none',5000);}
function size(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';if(b>=1048576)return(b/1048576).toFixed(0)+' MB';return(b/1024).toFixed(0)+' KB';}
function esc(s){return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
async function get(u){const r=await fetch(u,{cache:'no-store'});if(r.status==401){showLogin();throw 0;}return r.json();}
function showLogin(){$('login').style.display='';$('app').style.display='none';}
async function login(){const r=await fetch('/api/login?pin='+encodeURIComponent($('pin').value),{method:'POST'});const j=await r.json();
 if(j.ok){$('login').style.display='none';$('app').style.display='';refresh(true);}else toast(j.msg,true);}
async function act(u,confirmMsg){if(busy)return;if(confirmMsg&&!confirm(confirmMsg))return;busy=true;toast('Working... (see the DS)');
 try{const r=await fetch(u,{method:'POST'});if(r.status==401){showLogin();return;}const j=await r.json();toast(j.msg,!j.ok);}
 catch(e){toast('No answer from the DS',true);}finally{busy=false;refresh(true);}}
function q(n){return encodeURIComponent(n);}
function mount(n,m){act('/api/mount?name='+q(n)+'&mode='+m);}
function del(n){act('/api/delete?name='+q(n),'Delete '+n+' from the SD card?');}
function unmount(){if(st&&!st.safe&&st.busy){if(!confirm('The PC is using the drive. Eject it on the PC first. Unmount anyway?'))return;act('/api/unmount?force=1');}
 else act('/api/unmount?force='+(st&&!st.safe?'1':'0'));}
function createImg(){const n=$('newname').value.trim();if(!n){toast('Enter a name',true);return;}act('/api/create?name='+q(n)+'&mib='+$('newsize').value);}
function renderStatus(s){st=s;let h='';
 if(s.sdShared)h='<div class="warn"><b>Whole SD card is shared with the PC.</b></div><div class="dim">Browsing is paused until it is unmounted.</div>';
 else if(s.mounted)h='<div class="ok"><b>Mounted:</b> <span class="name">'+esc(s.name)+'</span></div><div class="dim">'+(s.cd?'CD/DVD drive':(s.readOnly?'Read-only disk':'Disk'))+' &middot; '+esc(s.usb)+(s.readKBs?' &middot; reading '+s.readKBs+' KB/s':'')+'</div>';
 else h='<div><b>Nothing mounted</b></div><div class="dim">Pick an image below.</div>';
 if(s.mounted){h+='<div class="row" style="margin-top:10px"><button class="primary" onclick="unmount()">Unmount</button>';
  if(s.hostEjected)h+='<button onclick="act(\'/api/reinsert\')">Reinsert</button>';
  h+='<span class="dim">'+(s.safe?'Safe to unmount':(s.busy?'PC is using it: eject there first':'Idle'))+'</span></div>';}
 else h+='<div class="row" style="margin-top:10px"><button onclick="act(\'/api/sharesd\',\'Share the whole SD card with the PC? The DS cannot browse until it is unmounted.\')">Share whole SD card</button></div>';
 h+='<div class="dim" style="margin-top:8px">SD: '+esc(s.fs||'?')+' &middot; largest new image '+s.maxMiB+' MB'+(s.maxMiB<s.fsMaxMiB?' (free space)':' ('+esc(s.fs||'file')+' limit)')+'</div>';
 $('status').innerHTML=h;
 for(const o of $('newsize').options){o.disabled=+o.value>s.maxMiB;}
 const sel=$('newsize');if(sel.selectedOptions[0]&&sel.selectedOptions[0].disabled){for(let i=sel.options.length-1;i>=0;i--)if(!sel.options[i].disabled){sel.selectedIndex=i;break;}}}
function renderImages(list){let h='';const locked=st&&(st.mounted||st.sdShared);
 if(!list.length)h='<div class="dim">No images yet.</div>';
 for(const e of list){h+='<div class="img"><div class="row"><div class="grow"><div class="name">'+esc(e.name)+(e.mounted?' <span class="ok">(mounted)</span>':'')+'</div><div class="dim">'+size(e.size)+(e.kind?' &middot; '+esc(e.kind):'')+'</div></div></div><div class="row" style="margin-top:6px">';
  const n=esc(e.name.replace(/\\/g,'\\\\').replace(/'/g,"\\'"));
  if(e.cue)h+='<button class="primary" '+(locked?'disabled':'')+' onclick="mount(\''+n+'\',\'auto\')">Mount CD</button>';
  else{h+='<button class="primary" '+(locked?'disabled':'')+' onclick="mount(\''+n+'\',\'auto\')">'+(e.disc?'Mount as CD/DVD':'Mount')+'</button>';
   h+='<button '+(locked?'disabled':'')+' onclick="mount(\''+n+'\',\'ro\')">Read-only disk</button>';
   if(!e.disc)h+='<button '+(locked?'disabled':'')+' onclick="mount(\''+n+'\',\'cd\')">As CD/DVD</button>';}
  h+='<button class="danger" '+(locked?'disabled':'')+' onclick="del(\''+n+'\')">Delete</button></div></div>';}
 $('images').innerHTML=h;}
async function refresh(full){try{const s=await get('/api/status');renderStatus(s);if(full||!window._imgs){window._imgs=1;renderImages(await get('/api/images'));}
 $('login').style.display='none';$('app').style.display='';}catch(e){}}
refresh(true);setInterval(()=>{if(!busy)refresh(false);},2000);setInterval(()=>{if(!busy)refresh(true);},15000);
</script></body></html>
)HTML";

const size_t gWebPageSize = sizeof(gWebPage) - 1;
