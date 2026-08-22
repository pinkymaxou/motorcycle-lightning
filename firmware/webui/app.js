'use strict';
if(!CanvasRenderingContext2D.prototype.roundRect)
  CanvasRenderingContext2D.prototype.roundRect=function(x,y,w,h){this.rect(x,y,w,h);return this;};

/* ============ protobuf wire format (docs/ws_protocol.proto) ============ */
const TENC=new TextEncoder(),TDEC=new TextDecoder();
function pbW(){
  const b=[];
  const w={
    varint(v){v=Math.floor(v);for(;;){const x=v&0x7f;v=Math.floor(v/128);
      if(v){b.push(x|0x80)}else{b.push(x);return}}},
    tag(f,wt){w.varint(f*8+wt)},
    uint(f,v){if(!v)return;w.tag(f,0);w.varint(v)},
    uintAlways(f,v){w.tag(f,0);w.varint(v)},         /* explicit zero */
    bool(f,v){w.uint(f,v?1:0)},
    boolAlways(f,v){w.uintAlways(f,v?1:0)},
    bytesAlways(f,arr){w.tag(f,2);w.varint(arr.length);for(const x of arr)b.push(x)},
    bytes(f,arr){if(!arr.length)return;w.bytesAlways(f,arr)},
    str(f,s){w.bytes(f,Array.from(TENC.encode(s||'')))},
    packedF32(f,vals){if(!vals.length)return;w.tag(f,2);w.varint(vals.length*4);
      for(const v of vals)b.push(v&255,(v>>>8)&255,(v>>>16)&255,(v>>>24)&255)},
    out(){return new Uint8Array(b)}
  };
  return w;
}
function dvarint(u8,st){
  let v=0,s=1;
  for(;;){const b=u8[st.i++];v+=(b&0x7f)*s;if(!(b&0x80))return v;s*=128;}
}
function pbScan(u8,cb){
  const st={i:0};
  while(st.i<u8.length){
    const tag=dvarint(u8,st),f=Math.floor(tag/8),w=tag&7;
    if(w===0)cb(f,dvarint(u8,st),null);
    else if(w===2){const len=dvarint(u8,st);cb(f,null,u8.subarray(st.i,st.i+len));st.i+=len;}
    else if(w===5)st.i+=4;
    else if(w===1)st.i+=8;
    else break;
  }
}
function packedF32(u8){
  const out=[];
  for(let i=0;i+4<=u8.length;i+=4)
    out.push((u8[i]|(u8[i+1]<<8)|(u8[i+2]<<16)|(u8[i+3]<<24))>>>0);
  return out;
}

/* ---- Config <-> JS object (proto/ws_protocol.proto) ---- */
const STRIP_COUNT = 2, MAX_SECTIONS = 8, MAX_LEDS = 300;

function emptySection(){
  /* fx_open is view state, not config: it keeps the per-event selects open
     for a section that still matches a preset. Never encoded. */
  return {led_count:0,reversed:false,turn:0,fx_open:false,
    idle:'',brake:'',turn_on:'',turn_off:'',aux:''};
}
function emptyStrip(){
  return {brightness:160,led_model:0,color_order:0,reversed:false,sections:[]};
}
function decodeSection(u8){
  const sec=emptySection();
  pbScan(u8,(f,v,s)=>{
    switch(f){
    case 1:sec.led_count=v;break;  case 2:sec.reversed=!!v;break;
    case 3:sec.turn=v;break;
    case 4:sec.idle=TDEC.decode(s);break;
    case 5:sec.brake=TDEC.decode(s);break;
    case 6:sec.turn_on=TDEC.decode(s);break;
    case 7:sec.turn_off=TDEC.decode(s);break;
    case 8:sec.aux=TDEC.decode(s);break;
    }
  });
  return sec;
}
function decodeStrip(u8){
  const st=emptyStrip();
  pbScan(u8,(f,v,s)=>{
    switch(f){
    case 2:st.brightness=v;break;  case 3:st.led_model=v;break;
    case 4:st.color_order=v;break; case 5:st.reversed=!!v;break;
    case 9:st.sections.push(decodeSection(s));break;
    }
  });
  return st;
}
function decodeConfig(u8){
  const c={strips:[],colors:[0,0,0,0],exit_x10:12,holdoff_s:0,
    sta:{ssid:'',active:false,pass_set:false}};
  pbScan(u8,(f,v,s)=>{
    switch(f){
    case 1:c.strips.push(decodeStrip(s));break;
    case 2:c.colors=packedF32(s);break;
    case 3:c.exit_x10=v;break;
    case 4:c.holdoff_s=v;break;
    case 5:pbScan(s,(ff,vv,ss)=>{
      if(ff===1)c.sta.ssid=TDEC.decode(ss);
      else if(ff===3)c.sta.active=!!vv;
      else if(ff===4)c.sta.pass_set=!!vv;});break;
    }
  });
  while(c.strips.length<STRIP_COUNT)c.strips.push(emptyStrip());
  return c;
}
function encodeSection(sec){
  const w=pbW();
  w.uint(1,sec.led_count);w.bool(2,sec.reversed);w.uint(3,sec.turn);
  w.str(4,sec.idle);w.str(5,sec.brake);
  w.str(6,sec.turn_on);w.str(7,sec.turn_off);w.str(8,sec.aux);
  return Array.from(w.out());
}
function encodeStrip(st){
  const w=pbW();
  w.uint(2,st.brightness);w.uint(3,st.led_model);
  w.uint(4,st.color_order);w.bool(5,st.reversed);
  /* every section keeps its slot: the order is the wiring order */
  for(const sec of st.sections)w.bytesAlways(9,encodeSection(sec));
  return Array.from(w.out());
}
function encodeConfig(c,staPass){
  const w=pbW();
  for(let i=0;i<STRIP_COUNT;i++)w.bytesAlways(1,encodeStrip(c.strips[i]));
  w.packedF32(2,c.colors);
  w.uintAlways(3,c.exit_x10);w.uintAlways(4,c.holdoff_s);
  const s=pbW();
  s.str(1,c.sta.ssid);s.str(2,staPass||'');s.boolAlways(3,c.sta.active);
  w.bytesAlways(5,Array.from(s.out()));
  return w.out();
}

/* ---- section helpers (geometry only; the module decides the lighting) ---- */
const totalLeds=st=>st.sections.reduce((a,s)=>a+(s.led_count|0),0);
function sectionRanges(st){
  let p=0;
  return st.sections.map(s=>{const r={start:p,end:p+(s.led_count|0)};p=r.end;return r;});
}
const TURN_LABEL=['—','LEFT','RIGHT'];
/* short role tag for legends and timeline lanes */
function sectionTag(sec){
  if(sec.turn)return TURN_LABEL[sec.turn][0];
  if(sec.brake)return 'B';
  if(sec.idle)return 'P';
  return '·';
}
function cmdTest(ev,active){
  const t=pbW();t.uintAlways(1,ev);t.boolAlways(2,active);
  const w=pbW();w.bytesAlways(1,Array.from(t.out()));return w.out();
}
function cmdOverride(active){const w=pbW();w.boolAlways(2,active);return w.out();}
function cmdRestore(){const w=pbW();w.boolAlways(3,true);return w.out();}

/* ======================= app state / API ============================ */
let cfg=null, fxIndex=[];
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
function toast(msg,err){const t=$('toast');t.textContent=msg;
  t.className=err?'err':'';t.style.display='block';
  clearTimeout(t._h);t._h=setTimeout(()=>t.style.display='none',3200);}
async function api(path,opt={}){
  const r=await fetch('/api/'+path,{method:opt.method||'GET',body:opt.body,
    headers:opt.body?{'Content-Type':'application/x-protobuf'}:undefined});
  if(!r.ok)throw(await r.text())||('HTTP '+r.status);
  return new Uint8Array(await r.arrayBuffer());
}
const CMD=b=>api('command',{method:'POST',body:b});
const EV={left:0,right:1,brake:2,aux:3};
/* fixed palette (FxColor order) */
const STRIP_LABEL=i=>`Strip ${i+1}`;
const COLOR_NAMES=['Position light','Brake','Turn signal','White'];
const TURN_AMBER=0xFF5A00FF,TURN_RED=0xFF0000FF,COLOR_TURN=2;

document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('nav button').forEach(x=>x.classList.remove('act'));
  document.querySelectorAll('section').forEach(x=>x.classList.remove('act'));
  b.classList.add('act');$('tab-'+b.dataset.tab).classList.add('act');
  if(b.dataset.tab==='system'||b.dataset.tab==='pinout')loadSysinfo();
});

/* Room reserved for the feed marker, on whichever end the data enters. */
const FEED_MARGIN=26;
/* LED 0 sits at the connector; "reversed" means the connector is the far
   end of the picture, so the drawing flips with it. */
function stripGeom(cv,n,feedLeft){
  return {x0:feedLeft?FEED_MARGIN:0, w:(cv.width-FEED_MARGIN)/n};
}
function ledX(cv,n,index,feedLeft){
  const g=stripGeom(cv,n,feedLeft);
  return g.x0+index*g.w;
}
/* The power/data connector: a plug at the edge and a lead into LED 0. */
function drawFeed(ctx,cv,feedLeft){
  const bodyW=9,bodyH=Math.min(20,cv.height-8);
  const x=feedLeft?4:cv.width-4-bodyW;
  const y=(cv.height-bodyH)/2;
  ctx.fillStyle='#ff9f1a';
  ctx.beginPath();ctx.roundRect(x,y,bodyW,bodyH,2);ctx.fill();
  ctx.strokeStyle='#ff9f1a';ctx.lineWidth=2;
  ctx.beginPath();
  ctx.moveTo(feedLeft?x+bodyW:x, cv.height/2);
  ctx.lineTo(feedLeft?FEED_MARGIN:cv.width-FEED_MARGIN, cv.height/2);
  ctx.stroke();
}
function drawStrip(cv,px,feedLeft=true){
  const ctx=cv.getContext('2d');
  ctx.fillStyle='#000';ctx.fillRect(0,0,cv.width,cv.height);
  const n=px.length,g=stripGeom(cv,n,feedLeft);
  for(let i=0;i<n;i++){
    const[r,g_,b]=px[i];
    ctx.fillStyle=`rgb(${r},${g_},${b})`;
    ctx.beginPath();
    ctx.roundRect(g.x0+i*g.w+g.w*0.12,4,g.w*0.76,cv.height-8,4);
    ctx.fill();
  }
  drawFeed(ctx,cv,feedLeft);
}

/* ============ 20 s debug timeline: one sample every 100 ms ============ */
const GRAPH_WINDOW_MS=20000,GRAPH_SAMPLE_MS=100;
const hist=[];
let lastStatus=null;
const lastFrame=[null,null];   /* dernière trame par ruban */
function zoneAvg(n,rgb,from,to){
  let r=0,g=0,b=0;
  from=Math.min(from,n);to=Math.min(to,n);
  const cnt=Math.max(1,to-from);
  for(let i=from;i<to;i++){r+=rgb[i*3];g+=rgb[i*3+1];b+=rgb[i*3+2];}
  return[r/cnt|0,g/cnt|0,b/cnt|0];
}
setInterval(()=>{
  if(!lastStatus)return;
  const s=lastStatus,now=performance.now();
  const sample={t:now,
    l:s.inputs.left_blink?(s.inputs.left_on?2:1):0,
    r:s.inputs.right_blink?(s.inputs.right_on?2:1):0,
    b:s.inputs.brake?2:0,a:s.inputs.aux?2:0,z:[]};
  if(cfg){
    sample.z=cfg.strips.map((st,i)=>{
      const f=lastFrame[i];
      if(!f||0===totalLeds(st))return null;
      return sectionRanges(st).map(r=>zoneAvg(f.n,f.rgb,r.start,r.end));
    });
  }
  hist.push(sample);
  while(hist.length&&hist[0].t<now-GRAPH_WINDOW_MS-1000)hist.shift();
},GRAPH_SAMPLE_MS);

const INPUT_LANES=['IN L','IN R','IN BRK','IN AUX'];
/* One lane per section, capped so a fully loaded config cannot grow the
   canvas without bound. */
const GRAPH_MAX_OUT_LANES=8;
function graphLanes(){
  const lanes=INPUT_LANES.slice();
  let out=0;
  if(cfg)cfg.strips.forEach((st,i)=>{
    st.sections.forEach((sec,k)=>{
      if(0===sec.led_count||out>=GRAPH_MAX_OUT_LANES)return;
      lanes.push(`S${i+1}.${k+1} ${sectionTag(sec)}`);
      out++;
    });
  });
  return lanes;
}
function drawGraph(){
  const cv=$('graphcanvas'),ctx=cv.getContext('2d');
  const lanes=graphLanes();
  const LANE_H=26;
  if(cv.height!==lanes.length*LANE_H){
    cv.height=lanes.length*LANE_H;
    cv.style.height=cv.height+'px';
  }
  const W=cv.width,H=cv.height,LM=52;
  const laneH=LANE_H;
  const now=performance.now();
  const x=t=>LM+(W-LM)*(1-(now-t)/GRAPH_WINDOW_MS);

  ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);
  ctx.strokeStyle='#1c2027';
  for(let s=0;s<=20;s+=5){
    const gx=x(now-s*1000);
    ctx.beginPath();ctx.moveTo(gx,0);ctx.lineTo(gx,H);ctx.stroke();
  }
  ctx.font='700 10px system-ui';
  lanes.forEach((label,i)=>{
    const y0=i*laneH;
    ctx.strokeStyle='#22262e';
    ctx.beginPath();ctx.moveTo(LM,y0+laneH-.5);ctx.lineTo(W,y0+laneH-.5);ctx.stroke();
    ctx.fillStyle='#8d95a2';
    ctx.fillText(label,8,y0+laneH/2+3);
  });

  const keys=['l','r','b','a'];
  for(let j=0;j<hist.length;j++){
    const cur=hist[j];
    const x1=Math.max(LM,x(cur.t));
    const x2=j+1<hist.length?x(hist[j+1].t):W;
    if(x2<LM)continue;
    const w=Math.max(1,x2-x1);
    keys.forEach((k,i)=>{
      if(cur[k]===0)return;
      const y0=i*laneH,h=laneH-5;
      ctx.fillStyle=cur[k]===2?'#ff9f1a':'rgba(255,159,26,.28)';
      const hh=cur[k]===2?h:h*.45;
      ctx.fillRect(x1,y0+laneH-3-hh,w,hh);
    });
    let lane=INPUT_LANES.length;
    (cur.z||[]).forEach((sections,si)=>{
      if(!sections)return;
      sections.forEach((zone,k)=>{
        if(lane>=lanes.length)return;
        const st=cfg&&cfg.strips[si];
        if(st&&st.sections[k]&&0===st.sections[k].led_count)return;
        const[r,g,b]=zone;
        if(r>=4||g>=4||b>=4){
          ctx.fillStyle=`rgb(${r},${g},${b})`;
          ctx.fillRect(x1,lane*laneH+2,w,laneH-5);
        }
        lane++;
      });
    });
  }
}
setInterval(()=>{
  if(document.querySelector('#tab-sim').classList.contains('act'))drawGraph();
},100);

/* ============ WebSocket push: WsMessage (Frame | Status) ============ */
function decodeStatus(u8){
  /* proto3 omits false booleans: spell the defaults out so absent fields
     read as false instead of undefined (undefined makes classList.toggle
     flip the class instead of forcing it). */
  const r={inputs:{left_blink:false,left_on:false,right_blink:false,
      right_on:false,brake:false,aux:false},
    period:0,learned:false,override:false,
    fps_x10:0,frame_us:0,heap:0,sta_count:0,sta_ip:'',warnings:'',fw:''};
  pbScan(u8,(f,v,s)=>{
    switch(f){
    case 1:{const names={1:'left_blink',2:'left_on',3:'right_blink',4:'right_on',5:'brake',6:'aux'};
      pbScan(s,(ff,vv)=>{if(names[ff])r.inputs[names[ff]]=!!vv;});break;}
    case 2:r.period=v;break;   case 3:r.learned=!!v;break;
    case 4:r.override=!!v;break;
    case 6:r.fps_x10=v;break;  case 7:r.frame_us=v;break;
    case 8:r.heap=v;break;     case 9:r.sta_count=v;break;
    case 10:r.sta_ip=TDEC.decode(s);break;
    case 11:r.warnings=TDEC.decode(s);break;
    case 12:r.fw=TDEC.decode(s);break;
    }
  });
  return r;
}
function renderLiveStrips(){
  const host=$('livestrips');host.innerHTML='';
  cfg.strips.forEach((st,i)=>{
    if(0===totalLeds(st))return;
    host.insertAdjacentHTML('beforeend',
      `<div class="striplabel">${STRIP_LABEL(i)} — ${totalLeds(st)} LEDs ·
         feed ${st.reversed?'right':'left'} (LED 1 at the connector)</div>
       <canvas class="strip" data-live="${i}" width="900" height="44"></canvas>
       <div class="row muted" style="margin:3px 0 0">
         ${st.sections.map((sec,k)=>
           `<span>${k+1}: ${sec.led_count} ${TURN_LABEL[sec.turn]==='—'?
             (sec.brake?'brake':'position'):TURN_LABEL[sec.turn].toLowerCase()+' turn'}`+
           `${sec.reversed?' ◂':''}</span>`).join(' · ')}
       </div>`);
  });
  if(!host.innerHTML)host.innerHTML='<p class="muted">No strip configured.</p>';
}
function drawFrame(strip,n,rgb){
  if(!document.querySelector('#tab-sim').classList.contains('act'))return;
  const cv=document.querySelector(`[data-live="${strip}"]`);
  if(!cv)return;
  const px=new Array(n);
  for(let i=0;i<n;i++)px[i]=[rgb[i*3],rgb[i*3+1],rgb[i*3+2]];
  const st=cfg&&cfg.strips[strip];
  const feedLeft=!(st&&st.reversed);
  drawStrip(cv,px,feedLeft);
  if(st){
    const ctx=cv.getContext('2d');
    ctx.strokeStyle='#555';ctx.setLineDash([3,3]);
    const ranges=sectionRanges(st);
    ranges.slice(0,-1).forEach(r=>{
      if(r.end<=0||r.end>=n)return;
      const x=ledX(cv,n,r.end,feedLeft);
      ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,cv.height);ctx.stroke();
    });
    ctx.setLineDash([]);
  }
}
let ws=null;
function wsConnect(){
  ws=new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType='arraybuffer';
  ws.onmessage=ev=>{
    const u8=new Uint8Array(ev.data);
    pbScan(u8,(f,v,s)=>{
      if(f===1&&s){ /* Frame */
        let n=0,rgb=null,strip=0;
        pbScan(s,(ff,vv,ss)=>{
          if(ff===1)n=vv;else if(ff===2)rgb=ss;else if(ff===3)strip=vv;});
        if(n&&rgb){drawFrame(strip,n,rgb);lastFrame[strip]={n,rgb};}
      }else if(f===2&&s){
        const st=decodeStatus(s);
        updateStatus(st);
        lastStatus=st;
      }
    });
  };
  ws.onopen=()=>{$('livestate').textContent='live';};
  ws.onclose=()=>{
    $('livestate').textContent='offline';
    $('linkdot').classList.remove('on');
    $('linkinfo').textContent='offline';
    setTimeout(wsConnect,2000);
  };
  ws.onerror=()=>ws.close();
}

/* ---- simulated signals: injected into the module's real pipeline ---- */
const sim={left:false,right:false,brake:false,aux:false};
async function sendForce(ev,active){
  try{await CMD(cmdTest(EV[ev],active));}
  catch(e){toast('module: '+e,true);}
}
document.querySelectorAll('[data-sim]').forEach(b=>{
  b.onclick=()=>{
    const w=b.dataset.sim;
    if(w==='hazard'){
      const on=!(sim.left&&sim.right);
      sim.left=sim.right=on;
      sendForce('left',on);sendForce('right',on);
    }else{
      sim[w]=!sim[w];
      sendForce(w,sim[w]);
    }
    document.querySelectorAll('[data-sim]').forEach(x=>{
      const k=x.dataset.sim;
      x.classList.toggle('on',k==='hazard'?(sim.left&&sim.right):sim[k]);
    });
  };
});
let overrideOn=false;
$('overridebtn').onclick=async()=>{
  overrideOn=!overrideOn;
  $('overridebtn').classList.toggle('on',overrideOn);
  try{await CMD(cmdOverride(overrideOn));}
  catch(e){toast('module: '+e,true);}
};
/* keep-alive: the module auto-clears forced events and override after 60 s */
setInterval(()=>{
  for(const ev of['left','right','brake','aux'])
    if(sim[ev])sendForce(ev,true);
  if(overrideOn)CMD(cmdOverride(true)).catch(()=>{});
},30000);

/* ---- effect index + shared settings ---- */
async function loadIndex(){
  const u8=await api('effects');
  fxIndex=[];
  pbScan(u8,(f,v,s)=>{
    if(f!==1||!s)return;
    const e={id:'',name:'',assigned:false};
    pbScan(s,(ff,vv,ss)=>{
      if(ff===1)e.id=TDEC.decode(ss);
      else if(ff===2)e.name=TDEC.decode(ss);
      else if(ff===3)e.assigned=!!vv;
    });
    fxIndex.push(e);
  });
  if(cfg)renderShared();
}
function renderShared(){
  /* Per-section effects live in the section editor above; these two come from
     the bike's own signals and apply to every strip. */
  $('holdoff').value=cfg.holdoff_s;
  const tc=(cfg.colors[COLOR_TURN]>>>0);
  $('turncolor').value=tc===TURN_AMBER?'amber':tc===TURN_RED?'red':'custom';
}

/* ---- fixed palette (values editable, set hardcoded) ---- */
function renderPalette(){
  const tb=document.querySelector('#palrows tbody');tb.innerHTML='';
  COLOR_NAMES.forEach((name,i)=>{
    const v=cfg.colors[i]>>>0;
    const r=(v>>>24)&255,g=(v>>>16)&255,b=(v>>>8)&255,a=v&255;
    const k=a/255;
    const hex='#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('');
    tb.insertAdjacentHTML('beforeend',`<tr>
      <td><span class="swatch" style="background:rgb(${r*k|0},${g*k|0},${b*k|0})"></span></td>
      <td>${name}</td>
      <td><input type="color" value="${hex}" data-pcol="${i}"></td>
      <td><input type="range" min="0" max="255" value="${a}" data-pal="${i}" style="width:110px"></td>
    </tr>`);
  });
  const upd=i=>{
    const hex=tb.querySelector(`[data-pcol="${i}"]`).value;
    const a=+tb.querySelector(`[data-pal="${i}"]`).value;
    cfg.colors[i]=((parseInt(hex.slice(1),16)*256)+a)>>>0;
    renderPalette();renderShared();
  };
  tb.querySelectorAll('[data-pcol]').forEach(e=>e.onchange=()=>upd(+e.dataset.pcol));
  tb.querySelectorAll('[data-pal]').forEach(e=>e.onchange=()=>upd(+e.dataset.pal));
}

/* ---- setup: hardware + section editor, per strip ---- */
const LED_MODELS=['WS2812 / WS2812B','SK6812','WS2811','WS2816 (16-bit)'];
const COLOR_ORDERS=['GRB','RGB','GRBW','RGBW'];

/* A preset only fills the selects below it — they stay the source of truth,
   and an id the module does not know is left empty rather than invented. */
const SECTION_PRESETS=[
  {id:'turn_l',label:'Turn left', turn:1,
   fx:{idle:'f_position',brake:'f_brake',turn_on:'f_turn_on',turn_off:'f_turn_off',aux:''}},
  {id:'turn_r',label:'Turn right',turn:2,
   fx:{idle:'f_position',brake:'f_brake',turn_on:'f_turn_on',turn_off:'f_turn_off',aux:''}},
  {id:'brake', label:'Brake + position',turn:0,
   fx:{idle:'f_position',brake:'f_brake',turn_on:'',turn_off:'',aux:''}},
  {id:'pos',   label:'Position only',turn:0,
   fx:{idle:'f_position',brake:'',turn_on:'',turn_off:'',aux:''}},
];
const FX_KEYS=['idle','brake','turn_on','turn_off','aux'];
const FX_LABELS={idle:'Idle / position',brake:'Brake',turn_on:'Turn ON',
  turn_off:'Turn off phase',aux:'Aux'};

const knownFx=id=>''===id||fxIndex.some(e=>e.id===id);
function matchPreset(sec){
  const p=SECTION_PRESETS.find(p=>p.turn===sec.turn&&
    FX_KEYS.every(k=>p.fx[k]===sec[k]));
  return p?p.id:'custom';
}
function applyPreset(sec,id){
  const p=SECTION_PRESETS.find(p=>p.id===id);
  if(!p)return;
  sec.turn=p.turn;
  FX_KEYS.forEach(k=>{sec[k]=knownFx(p.fx[k])?p.fx[k]:'';});
}

function renderSetup(){
  const host=$('setupstrips');host.innerHTML='';
  const opts=(list,sel)=>list.map((n,i)=>
    `<option value="${i}" ${i===sel?'selected':''}>${n}</option>`).join('');
  const fxopts=sel=>['<option value="">— none —</option>',
    ...fxIndex.map(e=>`<option value="${e.id}" ${sel===e.id?'selected':''}>${esc(e.name)}</option>`)].join('');

  cfg.strips.forEach((st,si)=>{
    const total=totalLeds(st);
    const over=total>MAX_LEDS||st.sections.length>MAX_SECTIONS;
    const rows=st.sections.map((sec,k)=>{
      const preset=matchPreset(sec);
      const custom='custom'===preset||sec.fx_open;
      const role=custom?'custom':preset;
      return `<tr class="sec${k%2}">
        <td class="secno">${k+1}</td>
        <td><input type="number" min="0" max="300" style="width:70px"
              data-sec="${si}:${k}:led_count" value="${sec.led_count}"></td>
        <td><select data-sec="${si}:${k}:reversed">
              <option value="0" ${sec.reversed?'':'selected'}>Forward ▸</option>
              <option value="1" ${sec.reversed?'selected':''}>◂ Reverse</option>
            </select></td>
        <td><select data-preset="${si}:${k}">
              ${SECTION_PRESETS.map(p=>
                `<option value="${p.id}" ${role===p.id?'selected':''}>${p.label}</option>`).join('')}
              <option value="custom" ${custom?'selected':''}>Custom…</option>
            </select></td>
        <td style="text-align:right;white-space:nowrap">
          <button class="btn" data-move="${si}:${k}:-1" ${0===k?'disabled':''}>↑</button>
          <button class="btn" data-move="${si}:${k}:1" ${k===st.sections.length-1?'disabled':''}>↓</button>
          <button class="btn danger" data-del="${si}:${k}">✕</button>
        </td>
      </tr>
      <tr class="sec${k%2}" data-custom="${si}:${k}" ${custom?'':'hidden'}>
        <td></td>
        <td colspan="4"><div class="row">
          <label>Turn source <select data-sec="${si}:${k}:turn">
            ${TURN_LABEL.map((t,i)=>
              `<option value="${i}" ${i===sec.turn?'selected':''}>${t}</option>`).join('')}
          </select></label>
          ${FX_KEYS.map(key=>`<label>${FX_LABELS[key]}
            <select data-sec="${si}:${k}:${key}">${fxopts(sec[key])}</select></label>`).join('')}
        </div></td>
      </tr>`;
    }).join('');

    host.insertAdjacentHTML('beforeend',`<div class="card">
      <h2>${STRIP_LABEL(si)}
        ${0===total?'<span class="pill">not installed</span>':''}</h2>
      <div class="grid2">
        <label>Brightness <input type="range" data-sp="${si}:brightness"
          min="5" max="255" value="${st.brightness}" style="width:130px">
          <span data-bv="${si}">${st.brightness}</span></label>
        <label>LED model <select data-sp="${si}:led_model">${opts(LED_MODELS,st.led_model)}</select></label>
        <label>Color order <select data-sp="${si}:color_order">${opts(COLOR_ORDERS,st.color_order)}</select></label>
        <label><input type="checkbox" data-sp="${si}:reversed"
          ${st.reversed?'checked':''}> reversed data direction</label>
      </div>
      <div class="striplabel">Sections — laid end to end from the connector
        <span class="muted" data-feed="${si}"></span></div>
      <canvas class="strip" data-zc="${si}" width="900" height="30"></canvas>
      <table><thead><tr>
        <th style="width:24px">#</th><th style="width:90px">LEDs</th>
        <th style="width:120px">Direction</th><th>Role</th><th></th>
      </tr></thead><tbody>${rows||
        '<tr><td colspan="5" class="muted">no section yet</td></tr>'}</tbody></table>
      <div class="row">
        <button class="btn" data-add="${si}"
          ${st.sections.length>=MAX_SECTIONS?'disabled':''}>＋ section</button>
        <span style="flex:1"></span>
        <span class="${over?'':'muted'}" style="${over?'color:var(--red)':''}"
          data-total="${si}">${total} / ${MAX_LEDS} LEDs ·
          ${st.sections.length} / ${MAX_SECTIONS} sections</span>
      </div>
    </div>`);
  });

  bindSetupHandlers();
  cfg.strips.forEach((_,si)=>drawSections(si));

  $('stassid').value=cfg.sta.ssid;
  $('staactive').checked=cfg.sta.active;
  $('stanote').textContent=cfg.sta.pass_set?'password saved':'no password';
  $('simsections').textContent=cfg.strips.map((st,i)=>
    0===totalLeds(st)?`${STRIP_LABEL(i)}: none`:
    `${STRIP_LABEL(i)}: ${totalLeds(st)} LEDs, `+
    st.sections.map(sec=>`${sec.led_count}${sectionTag(sec)}`).join(' | ')
  ).join(' · ');
}

function bindSetupHandlers(){
  const host=$('setupstrips');
  host.querySelectorAll('[data-sp]').forEach(el=>{
    const upd=()=>{
      const[si,key]=el.dataset.sp.split(':');
      const st=cfg.strips[+si];
      if(key==='reversed'){st.reversed=el.checked;drawSections(+si);}
      else{
        st[key]=+el.value;
        if(key==='brightness')host.querySelector(`[data-bv="${si}"]`).textContent=el.value;
      }
    };
    el.oninput=upd;el.onchange=upd;
  });
  host.querySelectorAll('[data-sec]').forEach(el=>{
    const upd=()=>{
      const[si,k,key]=el.dataset.sec.split(':');
      const sec=cfg.strips[+si].sections[+k];
      if(key==='led_count')sec.led_count=Math.max(0,Math.min(MAX_LEDS,+el.value||0));
      else if(key==='reversed')sec.reversed='1'===el.value;
      else if(key==='turn')sec.turn=+el.value;
      else sec[key]=el.value;
      if(key==='led_count'){drawSections(+si);refreshTotal(+si);}
      else renderSetup();      /* role changed: preset label follows */
    };
    el.onchange=upd;
    if(el.type==='number')el.oninput=()=>{
      const[si,k]=el.dataset.sec.split(':');
      cfg.strips[+si].sections[+k].led_count=
        Math.max(0,Math.min(MAX_LEDS,+el.value||0));
      drawSections(+si);refreshTotal(+si);
    };
  });
  host.querySelectorAll('[data-preset]').forEach(el=>el.onchange=()=>{
    const[si,k]=el.dataset.preset.split(':');
    const sec=cfg.strips[+si].sections[+k];
    sec.fx_open='custom'===el.value;
    if(!sec.fx_open)applyPreset(sec,el.value);
    renderSetup();
  });
  host.querySelectorAll('[data-move]').forEach(el=>el.onclick=()=>{
    const[si,k,d]=el.dataset.move.split(':').map(Number);
    const list=cfg.strips[si].sections;
    const to=k+d;
    if(to<0||to>=list.length)return;
    [list[k],list[to]]=[list[to],list[k]];   /* order is the wiring order */
    renderSetup();
  });
  host.querySelectorAll('[data-del]').forEach(el=>el.onclick=()=>{
    const[si,k]=el.dataset.del.split(':').map(Number);
    cfg.strips[si].sections.splice(k,1);
    renderSetup();
  });
  host.querySelectorAll('[data-add]').forEach(el=>el.onclick=()=>{
    const si=+el.dataset.add;
    const list=cfg.strips[si].sections;
    if(list.length>=MAX_SECTIONS)return;
    const sec=emptySection();
    applyPreset(sec,'brake');
    list.push(sec);
    renderSetup();
  });
}

function refreshTotal(si){
  const st=cfg.strips[si];
  const total=totalLeds(st);
  const el=document.querySelector(`[data-total="${si}"]`);
  if(!el)return;
  const over=total>MAX_LEDS;
  el.textContent=`${total} / ${MAX_LEDS} LEDs · ${st.sections.length} / ${MAX_SECTIONS} sections`;
  el.style.color=over?'var(--red)':'';
  el.className=over?'':'muted';
}

/* Section preview: one block per LED, colored by role, boundaries marked. */
const SECTION_COLORS={L:'#c86400',R:'#c86400',B:'#8c0000','P':'#3a0000','·':'#1a1a1a'};
const SECTION_COLOR_NONE='#1a1a1a';
/* Two sections with the same role would otherwise read as one run: every
   other section is drawn dimmer, matching the striped rows in the table. */
const SECTION_DIM=0.55;
function drawSections(si){
  const cv=document.querySelector(`[data-zc="${si}"]`);
  if(!cv)return;
  const st=cfg.strips[si],ctx=cv.getContext('2d');
  const feedLeft=!st.reversed;
  const n=totalLeds(st);
  ctx.fillStyle='#000';ctx.fillRect(0,0,cv.width,cv.height);
  const note=document.querySelector(`[data-feed="${si}"]`);
  if(note)note.textContent=`· feed ${feedLeft?'left':'right'}`;
  if(0===n){drawFeed(ctx,cv,feedLeft);return;}

  const g=stripGeom(cv,n,feedLeft);
  sectionRanges(st).forEach((r,k)=>{
    ctx.fillStyle=SECTION_COLORS[sectionTag(st.sections[k])]||SECTION_COLOR_NONE;
    ctx.globalAlpha=k%2?SECTION_DIM:1;
    for(let i=r.start;i<r.end;i++){
      ctx.beginPath();ctx.roundRect(g.x0+i*g.w+g.w*.12,4,g.w*.76,cv.height-8,3);
      ctx.fill();
    }
    ctx.globalAlpha=1;
    if(k<st.sections.length-1&&r.end>0&&r.end<n){
      const x=ledX(cv,n,r.end,feedLeft);
      ctx.strokeStyle='#8d95a2';ctx.setLineDash([3,3]);
      ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,cv.height);ctx.stroke();
      ctx.setLineDash([]);
    }
  });
  drawFeed(ctx,cv,feedLeft);
}

$('savesetup').onclick=async()=>{
  for(const[i,st]of cfg.strips.entries()){
    if(totalLeds(st)>MAX_LEDS||st.sections.length>MAX_SECTIONS){
      return toast(`${STRIP_LABEL(i)}: too many LEDs or sections`,true);
    }
  }
  cfg.holdoff_s=Math.max(0,Math.min(600,+$('holdoff').value||0));
  const turn=$('turncolor').value;
  if(turn!=='custom')cfg.colors[COLOR_TURN]=turn==='red'?TURN_RED:TURN_AMBER;
  cfg.sta.ssid=$('stassid').value.trim();
  cfg.sta.active=$('staactive').checked;
  const pass=$('stapass').value;   /* empty = keep current */
  try{
    await api('config',{method:'PUT',body:encodeConfig(cfg,pass)});
    $('stapass').value='';
    cfg=decodeConfig(await api('config'));
    renderAll();
    toast('saved ✓');
  }catch(e){toast(e,true);}
};
$('restorebtn').onclick=async()=>{
  if(!confirm('Restore factory sections and setup?'))return;
  try{
    await CMD(cmdRestore());
    cfg=decodeConfig(await api('config'));
    renderAll();toast('defaults restored');
  }catch(e){toast(e,true);}
};

/* ---- status (pushed over the WebSocket) ---- */
function updateStatus(s){
  $('linkdot').classList.add('on');
  $('linkinfo').textContent=(s.sta_ip?s.sta_ip+' · ':'')+(s.fps_x10/10).toFixed(0)+' fps';
  const ri=(id,act,onPhase)=>{
    const e=$(id);
    e.classList.toggle('on',!!act);
    e.classList.toggle('dimphase',!!act&&false===onPhase);
  };
  ri('ri-left',s.inputs.left_blink,s.inputs.left_on);
  ri('ri-right',s.inputs.right_blink,s.inputs.right_on);
  ri('ri-brake',s.inputs.brake);
  ri('ri-aux',s.inputs.aux);
  $('ri-note').textContent=
    `· flasher ${s.period} ms${s.learned?' (learned)':''}`+
    (s.override?' · OVERRIDE ACTIVE':'');
  $('modstatus').textContent=
    `fw ${s.fw} · ${(s.fps_x10/10).toFixed(1)} fps · frame max ${s.frame_us} µs · `+
    `heap ${(s.heap/1024).toFixed(0)} KB · ${s.sta_count} client(s)`+
    (s.warnings?` · ⚠ ${s.warnings}`:'');
}

/* ---- system info ---- */
function fmtUptime(s){
  const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
        m=Math.floor(s%3600/60),sec=Math.floor(s%60);
  const p=x=>String(x).padStart(2,'0');
  return `${d}d ${p(h)}h${p(m)}m${p(sec)}s`;
}
async function loadSysinfo(){
  try{
    const u8=await api('sysinfo');
    const si={pins:[]};
    const S={1:'chip',2:'fw',3:'compile',4:'sha',5:'idf',6:'mac_sta',7:'mac_ap',
      8:'mac_bt',11:'sta_ip',12:'ap_ip'};
    pbScan(u8,(f,v,s)=>{
      if(S[f]&&s)si[S[f]]=TDEC.decode(s);
      else if(f===9)si.heap_free=v;
      else if(f===10)si.heap_total=v;
      else if(f===13)si.uptime=v;
      else if(f===14&&s){
        const p={name:'',gpio:0,desc:''};
        pbScan(s,(ff,vv,ss)=>{
          if(ff===1)p.name=TDEC.decode(ss);
          else if(ff===2)p.gpio=vv;
          else if(ff===3)p.desc=TDEC.decode(ss);
        });
        si.pins.push(p);
      }
    });
    const rows=[
      ['Chip',si.chip],['Firmware',si.fw],['Compile time',si.compile],
      ['SHA256',si.sha],['IDF',si.idf],
      ['WiFi.STA',si.mac_sta],['WiFi.AP',si.mac_ap],['WiFi.BT',si.mac_bt],
      ['Memory',`${si.heap_free} / ${si.heap_total}`],
      ['WiFi (STA)',si.sta_ip||'—'],['WiFi (Soft-AP)',si.ap_ip],
      ['Uptime',fmtUptime(si.uptime||0)],
    ];
    document.querySelector('#systable tbody').innerHTML=rows.map(r=>
      `<tr><td class="muted" style="width:150px">${r[0]}</td>`+
      `<td style="word-break:break-all">${esc(r[1]??'—')}</td></tr>`).join('');

    document.querySelector('#pintable tbody').innerHTML=si.pins.map(p=>
      `<tr><td><b>${esc(p.name)}</b></td>`+
      `<td class="muted">G${p.gpio}</td>`+
      `<td class="muted">${esc(p.desc)}</td></tr>`).join('')||
      '<tr><td colspan="3" class="muted">no pin information</td></tr>';
  }catch(e){toast('sysinfo: '+e,true);}
}
$('sysrefresh').onclick=loadSysinfo;

/* ---- boot ---- */
function renderAll(){renderShared();renderPalette();renderSetup();renderLiveStrips();}
(async()=>{
  try{
    cfg=decodeConfig(await api('config'));
    await loadIndex();
    renderAll();
    loadSysinfo();
    wsConnect();
  }catch(e){
    toast('cannot reach module: '+e,true);
    $('linkinfo').textContent='offline';
  }
})();
