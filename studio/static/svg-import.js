/* Import planar geometry only. Uploaded elements never enter the live document. */
(() => {
  'use strict';
  const NS = 'http://www.w3.org/2000/svg';
  const NUM = /[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/y;
  const MM_PER_PX = 25.4 / 96;
  const units = {px:1, mm:1/MM_PER_PX, cm:10/MM_PER_PX, in:96, pt:96/72, pc:16, q:.25/MM_PER_PX};
  const shapes = new Set(['path','rect','line','circle','ellipse','polygon','polyline']);
  const ignored = new Set(['title','desc','metadata','defs','symbol']);
  const inertStyles = new Set(['display','visibility','opacity','fill','stroke','fill-opacity','stroke-opacity',
    'stroke-dasharray','stroke-dashoffset','clip-path','mask','filter','marker-start','marker-mid','marker-end',
    'stroke-width','stroke-linejoin','stroke-linecap','stroke-miterlimit','fill-rule','color','vector-effect']);
  const fail = message => { throw Error('SVG: ' + message); };
  const finite = n => Number.isFinite(n) && Math.abs(n) <= 1e12 ? n : fail('invalid or excessive coordinate.');
  function length(value, fallback=0) {
    if (value == null) return fallback;
    const m = String(value).trim().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)([a-z]*)$/i);
    if (!m || (m[2] && !Object.hasOwn(units,m[2].toLowerCase()))) fail('use numeric, px, mm, cm, Q, in, pt or pc lengths.');
    return finite(Number(m[1]) * (units[m[2].toLowerCase()] || 1));
  }

  class Cursor {
    constructor(text) { this.text = text; this.i = 0; }
    skip() { while (this.i < this.text.length && /[\s,]/.test(this.text[this.i])) ++this.i; }
    more() { this.skip(); return this.i < this.text.length; }
    number(flag=false) {
      this.skip();
      if (flag) {
        const c = this.text[this.i++];
        if (c !== '0' && c !== '1') fail('arc flags must be 0 or 1.');
        return Number(c);
      }
      NUM.lastIndex = this.i;
      const token = NUM.exec(this.text);
      if (!token) fail('missing or invalid path coordinate.');
      this.i = NUM.lastIndex;
      return finite(Number(token[0]));
    }
  }
  function numbers(text) {
    const cur = new Cursor(text), output = [];
    while (cur.more()) {
      if (output.length >= 200000) fail('too many coordinates.');
      output.push(cur.number());
    }
    return output;
  }
  // Keep engineering coordinates in double precision even on browsers whose
  // native graphics matrices round multiplications to float.
  class Affine {
    constructor(values=[1,0,0,1,0,0]) {
      [this.a,this.b,this.c,this.d,this.e,this.f]=values.map(finite);
    }
    multiply(m) {
      return new Affine([this.a*m.a+this.c*m.b,this.b*m.a+this.d*m.b,
        this.a*m.c+this.c*m.d,this.b*m.c+this.d*m.d,
        this.a*m.e+this.c*m.f+this.e,this.b*m.e+this.d*m.f+this.f]);
    }
    translate(x,y) {return this.multiply(new Affine([1,0,0,1,x,y]));}
    scale(x,y=x) {return this.multiply(new Affine([x,0,0,y,0,0]));}
    rotate(degrees) {
      const radians=degrees*Math.PI/180,c=Math.cos(radians),s=Math.sin(radians);
      return this.multiply(new Affine([c,s,-s,c,0,0]));
    }
    skewX(angle) {return this.multiply(new Affine([1,0,Math.tan(angle*Math.PI/180),1,0,0]));}
    skewY(angle) {return this.multiply(new Affine([1,Math.tan(angle*Math.PI/180),0,1,0,0]));}
  }
  function transform(text) {
    let matrix = new Affine(), cursor = 0;
    const parts = /\s*,?\s*([A-Za-z]+)\s*\(([^()]*)\)/gy;
    while (cursor < text.trimEnd().length) {
      parts.lastIndex = cursor;
      const part = parts.exec(text);
      if (!part) fail('invalid transform.');
      cursor = parts.lastIndex;
      const a = numbers(part[2]);
      let local = new Affine();
      switch (part[1]) {
        case 'matrix': if (a.length!==6) fail('invalid matrix.'); local = new Affine(a); break;
        case 'translate': if (a.length<1||a.length>2) fail('invalid translation.'); local = local.translate(a[0],a[1]||0); break;
        case 'scale': if (a.length<1||a.length>2) fail('invalid scale.'); local = local.scale(a[0],a[1]??a[0]); break;
        case 'rotate':
          if (a.length!==1 && a.length!==3) fail('invalid rotation.');
          local = local.translate(a[1]||0,a[2]||0).rotate(a[0]).translate(-(a[1]||0),-(a[2]||0)); break;
        case 'skewX': case 'skewY':
          if (a.length!==1) fail('invalid skew.'); local=local[part[1]](a[0]); break;
        default: fail('unsupported transform '+part[1]+'.');
      }
      matrix = matrix.multiply(local);
      if (![matrix.a,matrix.b,matrix.c,matrix.d,matrix.e,matrix.f].every(Number.isFinite)) fail('invalid transform.');
    }
    return matrix;
  }

  function viewport(root,physical) {
    const hasBox = root.hasAttribute('viewBox');
    const view = hasBox ? numbers(root.getAttribute('viewBox')) : null;
    if (view && (view.length!==4 || view[2]<=0 || view[3]<=0)) fail('invalid viewBox.');
    const dimension=key=>{
      const value=root.getAttribute(key);
      if(value?.trim().endsWith('%')){
        if(physical)fail('original page scale needs absolute page lengths or a viewBox without percentage sizing.');
        return null;
      }
      return length(value,null);
    };
    let w=dimension('width'), h=dimension('height');
    if (view && w==null && h!=null) w=h*view[2]/view[3];
    if (view && h==null && w!=null) h=w*view[3]/view[2];
    w=(w??view?.[2]??300)*MM_PER_PX; h=(h??view?.[3]??150)*MM_PER_PX;
    if (!(w>0&&h>0)) fail('the page must have a positive width and height.');
    let matrix = new Affine().scale(MM_PER_PX);
    if(view) {
      let sx=w/view[2], sy=h/view[3], tx=0, ty=0;
      const aspect=(root.getAttribute('preserveAspectRatio')||'xMidYMid meet').trim().split(/\s+/);
      if (aspect.length>2 || !/^(?:none|x(?:Min|Mid|Max)Y(?:Min|Mid|Max))$/.test(aspect[0]) ||
          (aspect[1] && !['meet','slice'].includes(aspect[1]))) fail('invalid preserveAspectRatio.');
      if (aspect[0]!=='none') {
        sx=sy=(aspect[1]==='slice'?Math.max:Math.min)(sx,sy);
        const align=s=>s==='Mid'?.5:s==='Max'?1:0;
        tx=(w-view[2]*sx)*align(aspect[0].slice(1,4));
        ty=(h-view[3]*sy)*align(aspect[0].slice(5,8));
      }
      matrix=new Affine().translate(tx,ty).scale(sx,sy).translate(-view[0],-view[1]);
    }
    return {width:w,height:h,matrix};
  }

  // Emit one segment per SVG command so a point always lands on each real corner.
  function subpaths(data, budget) {
    const c=new Cursor(data), result=[];
    let x=0,y=0,sx=0,sy=0,cmd='',prev='',control=null,active=null;
    const point=(rel)=>{const a=c.number(),b=c.number();return rel?[finite(x+a),finite(y+b)]:[a,b];};
    while(c.more()) {
      if (++budget.tokens>110000) fail('simplify the number of segments.');
      if (/[A-Za-z]/.test(c.text[c.i])) cmd=c.text[c.i++];
      if (!cmd) fail('a path command is missing.');
      const kind=cmd.toUpperCase(),rel=cmd!==kind;
      if(kind==='M') {
        [x,y]=point(rel);sx=x;sy=y;active=[];result.push(active);prev='M';control=null;cmd=rel?'l':'L';continue;
      }
      if (!active) fail('start each path with a moveto.');
      if(prev==='Z' && kind!=='Z'){ active=[];result.push(active); }
      let end,curve=null,nextControl=null;
      const reflected = () => [2*x-control[0],2*y-control[1]];
      switch(kind) {
        case 'L': end=point(rel);break;
        case 'H': {const a=c.number();end=[finite((rel?x:0)+a),y];break;}
        case 'V': {const a=c.number();end=[x,finite((rel?y:0)+a)];break;}
        case 'Z': end=[sx,sy];cmd='';break;
        case 'C': case 'S': {
          const first=kind==='C'?point(rel):['C','S'].includes(prev)?reflected():[x,y];
          nextControl=point(rel);end=point(rel);curve='C'+[...first,...nextControl,...end].join(' ');break;
        }
        case 'Q': case 'T': {
          nextControl=kind==='Q'?point(rel):['Q','T'].includes(prev)?reflected():[x,y];
          end=point(rel);curve='Q'+[...nextControl,...end].join(' ');break;
        }
        case 'A': {
          const rx=c.number(),ry=c.number(),rotation=c.number(),large=c.number(true),sweep=c.number(true);
          if(rx<0||ry<0)fail('arc radii must be nonnegative.');
          end=point(rel);
          if(rx&&ry)curve='A'+[rx,ry,rotation,large,sweep,...end].join(' ');
          break;
        }
        default: fail('unsupported path command '+kind+'.');
      }
      if (curve || x!==end[0] || y!==end[1]) active.push({from:[x,y],end,curve});
      [x,y]=end;control=nextControl;prev=kind;
    }
    return result.filter(path=>path.length);
  }

  function geometry(el) {
    const l=(key,fallback)=>length(el.getAttribute(key),fallback);
    if(el.localName==='path')return el.getAttribute('d')||'';
    if(el.localName==='line')return 'M'+[l('x1'),l('y1')].join(' ')+'L'+[l('x2'),l('y2')].join(' ');
    if(['polygon','polyline'].includes(el.localName)){
      const a=numbers(el.getAttribute('points')||'');
      if(!a.length)return '';
      if(a.length%2)fail('incomplete polygon point.');
      return 'M'+a.join(' ')+(el.localName==='polygon'?'Z':'');
    }
    if(el.localName==='rect'){
      const x=l('x'),y=l('y'),w=l('width'),h=l('height');
      let rx=l('rx',null),ry=l('ry',null);
      if(w<0||h<0||rx<0||ry<0)fail('negative rectangle dimension.');
      if(!w||!h)return '';
      [rx,ry]=[Math.min(rx??ry??0,w/2),Math.min(ry??rx??0,h/2)];
      if(!rx||!ry)return 'M'+[x,y]+'h'+w+'v'+h+'h'+(-w)+'Z';
      const corner='a'+[rx,ry,0,0,1]+' ';
      return 'M'+[x+rx,y]+'h'+(w-2*rx)+corner+[rx,ry]+'v'+(h-2*ry)+corner+[-rx,ry]+
             'h'+(2*rx-w)+corner+[-rx,-ry]+'v'+(2*ry-h)+corner+[rx,-ry]+'Z';
    }
    const x=l('cx'),y=l('cy'),rx=l(el.localName==='circle'?'r':'rx'),ry=el.localName==='circle'?rx:l('ry');
    if(rx<0||ry<0)fail('negative radius.');if(!rx||!ry)return '';
    return 'M'+[x+rx,y]+'a'+[rx,ry,0,1,1,-2*rx,0]+'a'+[rx,ry,0,1,1,2*rx,0]+'Z';
  }

  function parse(text, options) {
    if(/<!DOCTYPE|<\?xml-stylesheet/i.test(text))fail('document declarations and external stylesheets are unsupported.');
    const doc=new DOMParser().parseFromString(text,'image/svg+xml'), root=doc.documentElement;
    if(doc.querySelector('parsererror')||root.localName!=='svg'||(root.namespaceURI && root.namespaceURI!==NS))fail('invalid SVG document.');
    const all=doc.getElementsByTagName('*');if(all.length>20000)fail('simplify the number of elements.');
    const ids=new Map();
    for(const el of all){
      if (['script','foreignObject','style','image','iframe','set','animate','animateMotion','animateTransform'].includes(el.localName))
        fail('remove active content, embedded images and stylesheets before importing.');
      for(const a of el.attributes) {
        if(/^on/i.test(a.localName) || (a.localName==='href' && (el.localName!=='use'||!/^#[^\s#]+$/.test(a.value))))
          fail('only local geometry references are supported.');
      }
      if(el.id){if(ids.has(el.id))fail('duplicate element ID.');ids.set(el.id,el);}
    }
    const page=viewport(root,options.physical), budget={tokens:0,visits:0}, lines=[];
    const bounds=[Infinity,Infinity,-Infinity,-Infinity];
    function extent(p){bounds[0]=Math.min(bounds[0],p[0]);bounds[1]=Math.min(bounds[1],p[1]);bounds[2]=Math.max(bounds[2],p[0]);bounds[3]=Math.max(bounds[3],p[1]);}
    function mapped(matrix,x,y){return [finite(matrix.a*x+matrix.c*y+matrix.e),finite(matrix.b*x+matrix.d*y+matrix.f)];}
    const measure=document.createElementNS(NS,'path');
    function vertex(segment,t){
      if(t===0)return segment.from;if(t===1)return segment.end;
      const p=measure.getPointAtLength(t*segment.length);return [p.x,p.y];
    }
    function visit(el, matrix, inherited, depth, refs) {
      if(++budget.visits>30000||depth>96)fail('geometry nesting or reference expansion is too large.');
      if(ignored.has(el.localName))return;
      if(el.namespaceURI && el.namespaceURI!==NS)fail('convert non-SVG content to paths.');
      const style={...inherited};
      for(const a of el.attributes)if(inertStyles.has(a.localName))style[a.localName]=a.value.trim();
      for(const field of (el.getAttribute('style')||'').split(';')){
        if(!field.trim())continue;
        const pos=field.indexOf(':'), key=field.slice(0,pos).trim().toLowerCase(), value=field.slice(pos+1).replace(/!important\s*$/i,'').trim();
        if(pos<1||!inertStyles.has(key))fail('expand unsupported inline style '+key+' to paths.');
        style[key]=value;
      }
      const opacity = (value) => {
        if(value==null)return 1;
        const raw=String(value), n=Number(raw.endsWith('%')?raw.slice(0,-1):raw);
        if(!Number.isFinite(n))fail('unsupported opacity.');
        return n/(raw.endsWith('%')?100:1);
      };
      if ((style.display||'').toLowerCase()==='none'||opacity(style.opacity)<=0)return;
      for(const key of ['clip-path','mask','filter','marker-start','marker-mid','marker-end','stroke-dasharray']){
        if(style[key] && style[key]!=='none')fail('expand '+key+' to paths.');
      }
      if(/url\(/i.test((style.fill||'')+' '+(style.stroke||'')))fail('expand paint servers to plain geometry.');
      matrix=matrix.multiply(transform(el.getAttribute('transform')||''));
      if(el.localName==='use'){
        const href=el.getAttribute('href')||el.getAttributeNS('http://www.w3.org/1999/xlink','href')||'';
        const target=ids.get(href.slice(1));
        if(!target||(!shapes.has(target.localName)&&target.localName!=='g')||refs.has(target))fail('unresolved, cyclic or unsupported local use reference.');
        const next=new Set(refs);next.add(target);
        return visit(target,matrix.translate(length(el.getAttribute('x')),length(el.getAttribute('y'))),style,depth+1,next);
      }
      if(el===root || el.localName==='g'){
        for(const child of el.children)visit(child,matrix,style,depth+1,refs);
        return;
      }
      if(['hidden','collapse'].includes((style.visibility||'').toLowerCase()))return;
      if(!shapes.has(el.localName))fail('convert '+el.localName+' to paths.');
      const invisible=(paint,alpha)=>['none','transparent'].includes(paint)||opacity(alpha)<=0;
      if(invisible(style.fill,style['fill-opacity']) && invisible(style.stroke??'none',style['stroke-opacity']))return;
      for(const path of subpaths(geometry(el),budget)){
        if(lines.length>=3000)fail('limit the file to 3,000 subpaths.');
        const drawable=[];
        for(const seg of path){
          if(seg.curve){
            seg.d='M'+seg.from.join(' ')+' '+seg.curve;
            measure.setAttribute('d',seg.d);seg.length=measure.getTotalLength();
            if(!(seg.length>=0&&Number.isFinite(seg.length)))fail('curve length could not be measured.');
            if(seg.length===0)continue;
            for(let k=1;k<8;k++)extent(mapped(matrix,...vertex(seg,k/8)));
          }
          extent(mapped(matrix,...seg.from));extent(mapped(matrix,...seg.end));drawable.push(seg);
        }
        if(drawable.length)lines.push({matrix,path:drawable});
      }
    }
    visit(root,page.matrix,{},0,new Set());
    if(!lines.length)fail('no visible supported geometry.');
    const [x,y,width,height]=options.box, physical=options.physical;
    const fitX=physical?1:width/(bounds[2]-bounds[0]||1),fitY=physical?1:height/(bounds[3]-bounds[1]||1);
    const paths=[];let count=0;
    for(const {matrix,path} of lines){
      const points=[mapped(matrix,...path[0].from)];
      // The matrix norm bounds how a local arc length can grow in the bed plane.
      const gain=Math.hypot(matrix.a*fitX,matrix.b*fitY,matrix.c*fitX,matrix.d*fitY);
      for(const segment of path){
        const n=segment.curve ? Math.max(1,Math.ceil(segment.length*gain/.25)) : 1;
        if(count+points.length+n>100000)fail('simplify curves to fewer than 100,000 sampled vertices.');
        if(segment.curve)measure.setAttribute('d',segment.d);
        for(let j=1;j<=n;j++)points.push(mapped(matrix,...vertex(segment,j/n)));
      }
      count+=points.length;paths.push(points);
    }
    // Fit against the final, sampled extents so the requested dimensions are exact.
    bounds.splice(0,4,Infinity,Infinity,-Infinity,-Infinity);
    for(const path of paths)for(const pt of path)extent(pt);
    if((physical || /\bslice\b/.test(root.getAttribute('preserveAspectRatio')||'')) && (bounds[0]<-1e-6 || bounds[1]<-1e-6||bounds[2]>page.width+1e-6||bounds[3]>page.height+1e-6))
      fail('geometry extends beyond the original page. Expand the page or fit the artwork to a box.');
    const sx=width/(bounds[2]-bounds[0]||1),sy=height/(bounds[3]-bounds[1]||1);
    for(const path of paths)for(const pt of path){
      pt[0]=physical?x+pt[0]:x+(pt[0]-bounds[0])*sx;
      pt[1]=physical?y+page.height-pt[1]:y+(bounds[3]-pt[1])*sy;
      if(pt[0]<-1e-7||pt[1]<-1e-7||pt[0]>options.bed[0]+1e-7||pt[1]>options.bed[1]+1e-7)fail('imported artwork exceeds the work area. Change the placement, work area or sizing mode.');
    }
    return paths;
  }
  globalThis.KerfKiteSvg={parse};
})();
