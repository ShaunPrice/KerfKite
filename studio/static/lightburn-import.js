// LightBurn interchange. Work exclusively with inert parsed data and millimetres.
(() => {
  'use strict';
  const num = (v, label) => {
    if (v == null || String(v).trim() === '' || !Number.isFinite(Number(v))) throw Error('Invalid '+label);
    return Number(v);
  };
  const yes = v => /^(1|true)$/i.test(String(v));
  const direct = (el, name) => [...el.children].filter(n=>n.tagName===name);
  const attr = (el,k) => el.getAttribute(k);
  const coord = (el,k) => num(attr(el,k),k);
  const identity = [1,0,0,1,0,0];
  const apply = ([a,b,c,d,e,f],[x,y]) => [a*x+c*y+e,b*x+d*y+f];
  function matrix(parent,el) {
    const xf = direct(el,'XForm')[0];
    if (!xf) return parent;
    const m=xf.textContent.trim().split(/\s+/).map(v=>num(v,'transform'));
    if(m.length!==6) throw Error('Invalid LightBurn transform');
    const [a,b,c,d]=parent, [e,f]=apply(parent,m.slice(4));
    return [a*m[0]+c*m[1],b*m[0]+d*m[1],a*m[2]+c*m[3],b*m[2]+d*m[3],e,f];
  }
  const values = el => Object.fromEntries([...el.children].filter(n=>n.hasAttribute('Value')).map(n=>[n.tagName,attr(n,'Value')]));
  const palette=['#202020','#0000ff','#ff0000','#009000','#cdad00','#ff6600','#00aaaa','#ff00ff','#888888','#526da4','#ff6666','#49a849','#b39550','#9d629c','#e4a5b4','#9f822c'];
  function read(name,text,{maxPower=1000}={}) {
    const ext=name.toLowerCase().split('.').pop(), warnings=new Set();
    const result={objects:[],layers:[],presets:[],devices:[],warnings:[],mode:'laser'};
    const warn=s=>warnings.add(s);
    const limit=num(maxPower,'maximum S value');
    if(limit<=0||limit>100000) throw Error('Set the target controller’s maximum laser S value (1–100000)');
    function cut(raw,type) {
      const s={}; const map=(key,to,factor=1)=>{if(raw[key]!=null)s[to]=num(raw[key],key)*factor;};
      if(type==='Cut')s.operation='line';
      else if(type==='Scan')s.operation='fill';
      else warn('Unmapped layer mode: '+String(type||'unspecified').slice(0,50));
      map('speed','feed',60); map('maxPower','power',limit/100); map('numPasses','passes');
      if(type==='Scan'){map('interval','interval');map('angle','hatch_angle');if(raw.crossHatch!=null)s.crosshatch=Number(yes(raw.crossHatch));}
      const known=new Set(['type','index','name','speed','maxPower','numPasses','doOutput','hide','interval','angle','crossHatch']);
      for(const k of Object.keys(raw))if(!known.has(k))warn('Unmapped cut setting: '+k.slice(0,80));
      return s;
    }
    function preset(label,raw,type) {
      if(!['Cut','Scan'].includes(type)){warn('Skipped preset mode: '+String(type).slice(0,50));return;}
      result.presets.push({name:String(label||'Laser preset').slice(0,100),mode:'laser',settings:cut(raw,type)});
    }
    if(['lbdev','lbprefs'].includes(ext)){
      const d=JSON.parse(text);
      for(const row of d.DeviceList||[]){
        const w=num(row.Width,'bed width'),h=num(row.Height,'bed height');
        if(w<=0||h<=0||w>2000||h>2000){warn('Skipped unsupported bed size');continue;}
        result.devices.push({name:String(row.DisplayName||row.Name||'Laser').slice(0,80),width:w,height:h});
        for(const c of row.DefaultCutList||[])preset(c.name,c,c.type);
      }
      for(const c of d.CutList||[])preset(c.name,c,c.type);
      warn('Only bed dimensions and supported laser cut presets can be applied. Connection details, macros and application preferences are omitted.');
    } else if (['lbset','settings'].includes(ext)) {
      const vals={};
      for (const line of text.split(/\r?\n/)) {
        const match=line.trim().match(/^\$(30|130|131|132)\s*=\s*([\d.]+)(?:\s|$)/);
        if(match) vals[match[1]]=num(match[2],'GRBL setting');
      }
      if(!(vals[130]>0 && vals[130]<=2000 && vals[131]>0 && vals[131]<=2000))throw Error('This reader needs a GRBL text settings dump with $130 and $131 travel.');
      result.devices.push({name:'GRBL travel',width:vals[130],height:vals[131]});
      if(vals[30]>0) warn('Source maximum S value: '+vals[30]+'. Choose the receiving controller’s value for percentage conversion.');
      warn('Only XY travel is imported as a workspace preset. Firmware remains under controller setup.');
    } else {
      if(/<!DOCTYPE|<!ENTITY/i.test(text))throw Error('Unsupported XML declaration');
      const doc=new DOMParser().parseFromString(text,'application/xml'),root=doc.documentElement;
      if(doc.querySelector('parsererror')||doc.getElementsByTagName('*').length>60000)throw Error('Invalid or oversized LightBurn XML');
      if(root.tagName==='LightBurnLibrary'){
        for(const material of direct(root,'Material'))for(const entry of direct(material,'Entry'))for(const c of direct(entry,'CutSetting')){
          preset([attr(material,'name'),attr(entry,'Thickness'),attr(entry,'Desc')].filter(Boolean).join(' · '),values(c),attr(c,'type'));
        }
      }else if(root.tagName==='LightBurnProject'){
        const layerMap=new Map(), verts=new Map(), prims=new Map();
        function layer(key,raw={},type=''){
          if(layerMap.has(key))return layerMap.get(key);
          const row={key,name:String(raw.name||('C'+key.padStart(2,'0'))).slice(0,80),color:palette[Number(key)%palette.length]||'#525c65',
            output:raw.doOutput===undefined||yes(raw.doOutput),visible:!yes(raw.hide),settings:cut(raw,type)};
          if(type==='Tool'||type==='Image'||!['Cut','Scan'].includes(type))row.output=false;
          layerMap.set(key,row);result.layers.push(row);return row;
        }
        for(const c of direct(root,'CutSetting')){const v=values(c);layer(String(v.index??result.layers.length),v,attr(c,'type'));}
        let pointCount=0;
        const addPoint=(path,p)=>{if(++pointCount>100000||!p.every(Number.isFinite))throw Error('Invalid or oversized geometry');path.push(p);};
        const pointRE='[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[Ee][+-]?\\d+)?';
        const vertexRE=new RegExp('^('+pointRE+')\\s+('+pointRE+')((?:c[01][xy]'+pointRE+')*)S?$');
        function packed(el) {
          const vs=direct(el,'VertList')[0], ps=direct(el,'PrimList')[0];
          const vid=attr(el,'VertID'),pid=attr(el,'PrimID');
          let v=vid==null?undefined:verts.get(vid),p=pid==null?undefined:prims.get(pid);
          if(vs){
            const chunks=vs.textContent.trim().split('V');if(chunks.shift()!=='')throw Error('Invalid vertex list');
            v=chunks.map(chunk=>{const m=chunk.trim().match(vertexRE);if(!m)throw Error('Unsupported packed vertex');return {x:+m[1],y:+m[2],...Object.fromEntries([...m[3].matchAll(new RegExp('(c[01][xy])('+pointRE+')','g'))].map(a=>[a[1],+a[2]]))};});
          }else if(direct(el,'V').length)v=direct(el,'V').map(n=>({x:coord(n,'vx'),y:coord(n,'vy'),...Object.fromEntries(['c0x','c0y','c1x','c1y'].filter(k=>n.hasAttribute(k)).map(k=>[k,coord(n,k)]))}));
          if(ps)p=ps.textContent.trim();
          else if(direct(el,'P').length)p=direct(el,'P').map(n=>attr(n,'T')+coord(n,'p0')+' '+coord(n,'p1')).join('');
          if(!v||!p)throw Error('Missing vertex or primitive data');
          if(vid!=null)verts.set(vid,v);if(pid!=null)prims.set(pid,p);
          return [v,p];
        }
        function paths(el,m) {
          const [v,p]=packed(el),point=i=>{if(!v[i])throw Error('Invalid primitive index');return apply(m,[v[i].x,v[i].y]);};
          if(p==='LineClosed'||p==='LineOpen'){const line=[];v.forEach((_,i)=>addPoint(line,point(i)));if(p==='LineClosed'&&line.length)addPoint(line,line[0]);return [line];}
          const segments=[],rx=/\s*([LB])(\d+)\s+(\d+)\s*/gy; let cursor=0,last=-1,line=[];
          while(cursor<p.length){
            const seg=rx.exec(p);if(!seg)throw Error('Unsupported path primitive');
            cursor=rx.lastIndex;const i=+seg[2],j=+seg[3],a=point(i),b=point(j);
            if(last!==i||!line.length){line=[];segments.push(line);addPoint(line,a);}
            if(seg[1]==='B'){
              const cp=(n,k,fallback)=>n[k+'x']!=null&&n[k+'y']!=null?apply(m,[n[k+'x'],n[k+'y']]):fallback;
              const c=cp(v[i],'c0',a),d=cp(v[j],'c1',b);
              // Bound chord error using the maximum norm of the cubic's second derivative.
              const curvature=Math.max(...[0,1].map(t=>{const [u,w,y]=t?[c,d,b]:[a,c,d];return Math.hypot(u[0]-2*w[0]+y[0],u[1]-2*w[1]+y[1]);}));
              const steps=Math.max(1,Math.ceil(Math.sqrt(6*curvature/(8*.03))));
              if(!Number.isFinite(steps)||steps>10000)throw Error('Curve exceeds drawing limits');
              for(let k=1;k<steps;k++){const t=k/steps,u=1-t;addPoint(line,[0,1].map(n=>u**3*a[n]+3*u*u*t*c[n]+3*u*t*t*d[n]+t**3*b[n]));}
            }
            addPoint(line,b);last=j;
            if(line.length>2 && line[0][0]===b[0] && line[0][1]===b[1])last=-1;
          }
          return segments;
        }
        function visit(el,parent,depth=0){
          if(depth>60||result.objects.length>=1000)throw Error('Project exceeds nesting or object limits');
          const kind=attr(el,'Type'),m=matrix(parent,el),key=attr(el,'CutIndex')||'0';
          if(kind==='Group'){for(const children of direct(el,'Children'))for(const child of direct(children,'Shape'))visit(child,m,depth+1);return;}
          let ps;
          if(kind==='Path')ps=paths(el,m);
          else if(kind==='Text'&&direct(el,'BackupPath')[0]){
            const backup=direct(el,'BackupPath')[0];
            ps=paths(backup,direct(backup,'XForm').length?matrix(parent,backup):m);warn('Text imported as saved outlines.');
          }else if(kind==='Rect'||kind==='Ellipse'){
            const w=kind==='Rect'?coord(el,'W')/2:coord(el,'Rx'),h=kind==='Rect'?coord(el,'H')/2:coord(el,'Ry');
            if(!(w>0&&h>0))throw Error('Invalid shape dimensions');
            const r=kind==='Rect'?Math.min(w,h,num(attr(el,'Cr')||0,'corner radius')):0;
            if(r<0)throw Error('Invalid corner radius');
            const line=[];
            if(kind==='Rect'&&!r){for(const pt of [[-w,-h],[w,-h],[w,h],[-w,h]])addPoint(line,apply(m,pt));}
            else{
              const quadrants=kind==='Rect'?[[w-r,h-r],[-w+r,h-r],[-w+r,-h+r],[w-r,-h+r]]:[[0,0],[0,0],[0,0],[0,0]];
              const rx=r||w,ry=r||h,scaled=Math.hypot(...m.slice(0,4))*Math.max(rx,ry);
              const steps=Math.max(3,Math.ceil(Math.PI/2*Math.sqrt(scaled/(8*.03))));
              if(!Number.isFinite(steps)||steps>25000)throw Error('Shape exceeds drawing limits');
              quadrants.forEach(([x,y],q)=>{for(let i=0;i<=steps;i++){const a=(q+i/steps)*Math.PI/2;addPoint(line,apply(m,[x+rx*Math.cos(a),y+ry*Math.sin(a)]));}});
            }
            addPoint(line,line[0]);ps=[line];
          }else{warn('Skipped shape: '+String(kind).slice(0,60));return;}
          if(ps.some(path=>path.length<2))throw Error('Incomplete path');
          const row=layer(key);result.objects.push({name:(attr(el,'Name')||attr(el,'Str')||kind).slice(0,80),sourceLayer:row.key,paths:ps});
        }
        for(const el of direct(root,'Shape'))visit(el,identity);
        if(yes(attr(root,'MirrorX'))||yes(attr(root,'MirrorY')))warn('Source uses a mirrored device origin. Coordinates retain the source frame. Check orientation.');
        if(root.querySelector('SubLayer'))warn('Sublayers require manual CAM setup.');
      }else throw Error('Unrecognised LightBurn XML document');
    }
    if(!result.objects.length&&!result.devices.length&&!result.presets.length)throw Error('No supported artwork or settings found'+(warnings.size?' ('+[...warnings].join(', ')+')':''));
    result.warnings=[...warnings];return result;
  }
  window.CNCLightBurn={read};
})();
