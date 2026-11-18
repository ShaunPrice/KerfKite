/* View changes operate on cameras only. The SVG viewBox is also the drawing tools' coordinate frame. */
(() => {
  'use strict';
  const canvas = document.getElementById('canvas');
  const solid = document.getElementById('solid-canvas');
  const workspace = canvas.closest('.workspace');
  const solidPanel = document.getElementById('design-panel-solid');
  const orientations = ['iso','top','bottom','front','back','right','left'];
  let box = null;
  let bedWidth = 0, bedHeight = 0;

  function dimensions() {
    return [Number($('width').value)||300, Number($('height').value)||180];
  }
  function syncBed(width,height) {
    if (!box || width!==bedWidth || height!==bedHeight) {
      bedWidth=width; bedHeight=height;
      box=[-12,-12,width+24,height+24];
    }
  }
  function drawingBox(width,height) {
    syncBed(width,height);
    return box.join(' ');
  }
  function updateDrawing() {
    canvas.setAttribute('viewBox',drawingBox(...dimensions()));
    renderSelection(); // Keep handles a fixed size on screen after zoom.
  }
  function current() {
    return solidPanel.hidden ? '2d' : '3d';
  }
  function state(target=current()) {
    if (target==='3d') return {workspace:target,...window.CNCSolidCamera.state()};
    syncBed(...dimensions());
    return {workspace:'2d',viewBox:box.slice()};
  }
  function fitDrawing() {
    let left=Infinity, right=-Infinity, top=-Infinity, bottom=Infinity;
    const layers=window.CNCDesignLayers.layers();
    for (const object of objects) {
      if (!object.paths.length || !CNCDesignLayers.layerFor(object,layers).visible) continue;
      const [x,y,mx,my]=bounds(object.paths);
      left=Math.min(left,x); right=Math.max(right,mx);
      top=Math.max(top,my); bottom=Math.min(bottom,y);
    }
    if (!Number.isFinite(right)) return false;
    const w=Math.max(1,right-left)*1.15, h=Math.max(1,top-bottom)*1.15;
    box=[(left+right-w)/2,bedHeight-(bottom+top+h)/2,w,h];
    return true;
  }
  function zoomDrawing(factor, anchor) {
    const [x,y,w,h]=box;
    const minWidth=(bedWidth+24)/512, maxWidth=(bedWidth+24)*20;
    const next=Math.min(maxWidth,Math.max(minWidth,w/factor));
    const ratio=next/w;
    const [cx,cy]=anchor || [x+w/2,y+h/2];
    box=[cx+(x-cx)*ratio,cy+(y-cy)*ratio,next,h*ratio];
    updateDrawing();
  }
  function panDrawing(dx,dy) {
    const ctm=canvas.getScreenCTM();
    if (!ctm || !canvas.getBoundingClientRect().width) throw Error('Open the drawing viewport to pan.');
    const unit=1/Math.hypot(ctm.a,ctm.b);
    box[0]-=dx*unit; box[1]-=dy*unit;
    updateDrawing();
  }
  function apply(name,args=[],target='auto') {
    const spatial=orientations.includes(name)||name==='orbit';
    const drawing=name==='fit-bed'||name==='fit-drawing';
    const count={zoom:1,pan:2,orbit:2}[name]??0;
    if (!spatial && !drawing && !['zoom','pan','fit'].includes(name)) throw Error('Unknown view command.');
    if (!['auto','2d','3d'].includes(target)) throw Error('Choose the 2D drawing or 3D CAD workspace.');
    if (!Array.isArray(args)||args.length!==count||args.some(v=>!Number.isFinite(v)||Math.abs(v)>1e6))
      throw Error('Enter '+count+' finite numeric view argument'+(count===1?'':'s')+'.');
    if (name==='zoom' && args[0]<=0) throw Error('Zoom factor must be positive.');
    if (target==='auto') target=spatial?'3d':drawing?'2d':current();
    if ((target==='2d'&&spatial)||(target==='3d'&&drawing)) throw Error('This view belongs to the '+(spatial?'3D CAD':'2D drawing')+' workspace.');
    if (target!==current()) window.CNCToolTabs.open('design',target==='3d'?'solid':'shapes');
    if (target==='3d') return {workspace:target,...window.CNCSolidCamera.apply(name,args)};
    syncBed(...dimensions());
    if (name==='zoom') zoomDrawing(args[0]);
    else if (name==='pan') panDrawing(...args);
    else {
      if (name==='fit-bed' || !fitDrawing()) {
        if (name==='fit-drawing') throw Error('Add visible drawing geometry first.');
        box=null;
      }
      updateDrawing();
    }
    return state(target);
  }
  function restore(drawing,solidCamera){
    box=drawing.viewBox.slice();updateDrawing();window.CNCSolidCamera.restore(solidCamera);
  }
  window.KerfKiteViews=Object.freeze({apply,current,drawingBox,state,restore,paintMesh:window.CNCSolidCamera.paintMesh});

  function picker(parent,id,label,options,target) {
    const select=document.createElement('select');
    select.id=id;
    select.setAttribute('aria-label',label);
    select.add(new Option(label,''));
    for (const [value,title] of options) select.add(new Option(title,value));
    select.onchange=()=>{
      const [command,...args]=select.value.split(' ');
      select.selectedIndex=0;
      try {apply(command,args.map(Number),target);}
      catch (e) {message(e.message,true);}
      (target==='3d'?solid:canvas).focus({preventScroll:true});
    };
    parent.append(select);
  }
  picker(workspace.querySelector('.workbar'),'cad-view-drawing','Drawing view',[
    ['fit-bed','Fit bed'],['fit-drawing','Fit drawing'],['zoom 1.25','Zoom in (+)'],['zoom 0.8','Zoom out (−)']
  ],'2d');
  picker(workspace.querySelector('.solid-tools'),'cad-view-solid','View orientation',[
    ['iso','Isometric (Num 0)'],['front','Front (Num 1)'],['back','Back (Num 9)'],
    ['right','Right (Num 3)'],['left','Left (Num 4)'],['top','Top (Num 7)'],['bottom','Bottom (Num 2)'],
    ['fit','Fit to view (F)']
  ],'3d');
  canvas.setAttribute('tabindex','0');
  const drawingHint='Drawing viewport. Scroll to zoom around the pointer. Middle-drag or use arrow keys to pan. F fits visible artwork. Plus and minus zoom.';
  canvas.setAttribute('aria-label',drawingHint);
  canvas.setAttribute('title',drawingHint);
  solid.setAttribute('aria-label','3D CAD viewport. Drag or use arrows to orbit. Shift-drag, middle-drag or Shift+arrows to pan. F fits the model. Use the View orientation menu for standard views.');

  let pan=null;
  // Capture at the workspace so middle-drag never becomes a selection or a drawing gesture.
  workspace.addEventListener('pointerdown',e=>{
    if (!canvas.contains(e.target)) return;
    if (e.button===0) {canvas.focus({preventScroll:true});return;}
    if (e.button!==1 || pan) return;
    e.preventDefault();e.stopPropagation();
    canvas.focus({preventScroll:true});
    pan={id:e.pointerId,x:e.clientX,y:e.clientY};
    canvas.setPointerCapture(e.pointerId);
  },true);
  canvas.addEventListener('pointermove',e=>{
    if (!pan || pan.id!==e.pointerId) return;
    panDrawing(e.clientX-pan.x,e.clientY-pan.y);
    pan.x=e.clientX; pan.y=e.clientY;
  });
  canvas.addEventListener('lostpointercapture',()=>{pan=null;});
  canvas.addEventListener('wheel',e=>{
    if (e.ctrlKey || e.metaKey || pan || e.buttons) return;
    const ctm=canvas.getScreenCTM();
    if (!ctm) return;
    e.preventDefault();
    syncBed(...dimensions());
    const p=new DOMPoint(e.clientX,e.clientY).matrixTransform(ctm.inverse());
    const delta=e.deltaY*(e.deltaMode===1?16:e.deltaMode===2?canvas.clientHeight:1);
    zoomDrawing(Math.exp(-Math.max(-1000,Math.min(1000,delta))*.0015),[p.x,p.y]);
  },{passive:false});

  // Scope single-letter and navigation keys to the drawing and solid views. Tab lists, fields,
  // sketch dialogs and the separate CAM/assembly canvases keep their own keyboard behaviour.
  const numpad={Numpad0:'iso',Numpad1:'front',Numpad3:'right',Numpad7:'top',Numpad9:'back',Numpad4:'left',Numpad2:'bottom'};
  const arrows={ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]};
  function keydown(e) {
    if (e.defaultPrevented || e.isComposing || e.altKey || e.ctrlKey || e.metaKey) return;
    if (document.querySelector('dialog[open]') || document.body.inert) return;
    if (e.target!==canvas && e.target!==solid && e.target!==document.body) return;
    // The pendant owns global arrow keys while explicitly enabled. A focused CAD
    // viewport never sends an arrow to the machine, even if its operator enables jogging.
    if ($('pendantEnabled')?.checked) {
      if (arrows[e.key] && (e.target===canvas || e.target===solid)) {e.preventDefault();e.stopImmediatePropagation();}
      return;
    }
    const target=current();
    let name, args=[];
    if (numpad[e.code]) name=numpad[e.code];
    else if (e.key==='+'||e.key==='='||e.key==='-') {name='zoom';args=[e.key==='-'?.8:1.25];}
    else if (e.key.toLowerCase()==='f') name='fit';
    else if (arrows[e.key]) {
      name=target==='3d'&&!e.shiftKey?'orbit':'pan';
      const [x,y]=arrows[e.key];
      args=name==='orbit'?[x*10,-y*10]:[x*32,y*32];
    } else return;
    if (e.repeat && orientations.includes(name)) return;
    e.preventDefault();e.stopPropagation();
    apply(name,args,numpad[e.code]?'3d':target);
  }
  document.addEventListener('keydown',keydown,true);
  window.addEventListener('cnc-project-loaded',()=>{
    box=null;
    updateDrawing();
    window.CNCSolidCamera.apply('iso');
    window.CNCSolidCamera.apply('fit');
  });
})();
