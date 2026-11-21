/* Circuit board import: what the panel works out for itself. The geometry comes from the server
   (/api/pcb-import, studio/pcb_api.py); here are the request built from the panel's fields, the width a
   V-bit cuts, and the layers, objects and tools a reviewed board becomes in the design. No DOM. */
(function(root){
  'use strict';
  const round6=value=>Math.round(value*1e6)/1e6;
  const text=value=>String(round6(value));
  function number(value,name,low,high){
    if(typeof value!=='number'||!Number.isFinite(value))throw Error(name+' must be a number');
    if(value<low||value>high)throw Error(name+' must be between '+low+' and '+high);
    return value;
  }
  // The width a V-bit cuts at a depth: its flat tip, plus what the cone adds on both sides.
  function vBitWidth(tip,angle,depth){
    number(tip,'Tip width',0,10);number(angle,'V-bit angle',1,179);number(depth,'Engraving depth',0.000001,10);
    return round6(tip+2*depth*Math.tan(angle*Math.PI/360));
  }
  // A drilled hole as the editor holds one: a closed circle of the drill's diameter (drill-editor.js).
  function holeCircle(x,y,diameter){
    const r=diameter/2,circle=Array.from({length:32},(_,i)=>[x+r*Math.cos(i*Math.PI/16),y+r*Math.sin(i*Math.PI/16)]);
    return [...circle,circle[0].slice()];
  }
  // The body of the import request from the panel's values. files: {copper, outline, drill} as text or ''.
  function request(files,v){
    const body={};
    for(const key of ['copper','outline','drill']){
      const value=files[key];
      if(value==null||value==='')continue;
      if(typeof value!=='string')throw Error('A board file could not be read as text');
      if(value.length>4*1024*1024)throw Error('Board files up to 4 MiB can be imported');
      body[key]=value;
    }
    if(!Object.keys(body).length)throw Error('Choose a copper layer, a board outline or a drill file');
    // The server takes 8 MiB in one request.
    if(Object.values(body).reduce((n,value)=>n+value.length,0)>7*1024*1024)throw Error('The board files together are larger than 7 MiB');
    if(!['top','bottom'].includes(v.side))throw Error('Choose the top or the underside');
    body.side=v.side;
    body.width=number(v.width,'Work width',1,2000);body.height=number(v.height,'Work height',1,2000);
    const board=v.registered;
    if(board){
      // A second import registers with the first: the same offset, and the mirror line through the middle of
      // the same outline.
      body.offset=[board.offset[0],board.offset[1]];
      if(v.side==='bottom'&&!body.outline&&board.outline_bounds)body.mirror_x=round6((board.outline_bounds[0]+board.outline_bounds[2])/2);
    }else body.margin=[number(v.cornerX,'Board corner X',0,1000),number(v.cornerY,'Board corner Y',0,1000)];
    if(body.copper){
      const depth=number(v.depth,'Engraving depth',0.01,10);
      if(v.kind==='v')body.v_bit={tip_width:number(v.tip,'Tip width',0,10),angle:number(v.angle,'V-bit angle',10,170),depth};
      else if(v.kind==='width')body.tool_width=number(v.toolWidth,'Width of cut',0.01,10);
      else throw Error('Choose a V-bit or state the width of cut');
      body.passes=number(v.passes,'Passes',1,50);
      if(!Number.isInteger(body.passes))throw Error('Passes must be a whole number');
      body.overlap=round6(number(v.overlapPercent,'Overlap',0,90)/100);
      body.arc_tolerance=number(v.arcTolerance,'Arc tolerance',0.0005,0.1);
      if(v.direction==='conventional')body.direction='conventional';
      if(v.clip){
        if(!body.outline)throw Error('Cutting the isolation off at the outline needs the board outline');
        body.clip=true;
      }
    }
    const format={};
    if(v.units)format.units=v.units;
    if(v.zeros)format.omitted_zeros=v.zeros;
    if(v.integer!=null||v.decimal!=null){
      if(v.integer==null||v.decimal==null)throw Error('Give both the integer and the decimal digits of the drill file, or neither');
      format.integer=number(v.integer,'Integer digits',1,6);format.decimal=number(v.decimal,'Decimal digits',0,6);
    }
    if(body.drill&&Object.keys(format).length)body.drill_format=format;
    return body;
  }
  // How a drill file's numbers were read, in words, with where each part came from.
  function formatText(i){
    const from=source=>source==='decimal points'?'not needed: every number has a decimal point':source==='unused'?'not used':'from the '+source;
    const units=(i.units==='inch'?'inches':'millimetres')+' ('+from(i.units_from)+')';
    if(i.digits_from==='decimal points'||i.digits_from==='unused')return units+'; digits and zeros '+from(i.digits_from);
    return units+'; '+i.integer+'.'+i.decimal+' digits ('+from(i.digits_from)+'); '+i.omitted_zeros+' zeros omitted ('+from(i.zeros_from)+')';
  }
  // Whether the reading rests on a default: the case to show the correction fields for.
  const formatGuessed=i=>[i.digits_from,i.zeros_from].includes('default');
  // What was understood, a line for each file.
  function summary(answer){
    const lines=[],span=b=>text(b[2]-b[0])+' × '+text(b[3]-b[1])+' mm';
    if(answer.copper){
      const c=answer.copper,n=c.layer.counts;
      lines.push('Copper'+(c.layer.file_function?' ('+c.layer.file_function+')':'')+': '+n.flash+' flashes, '+n.line+' lines, '+n.arc+' arcs, '+n.region+' regions in '+
        (c.layer.units==='inch'?'inches':'millimetres')+'; '+c.islands+' separate pieces of copper; '+c.path_count+' contours for a tool cutting '+text(c.tool_width)+' mm wide');
    }
    if(answer.outline)lines.push('Outline: '+answer.outline.paths.length+(answer.outline.paths.length===1?' closed loop, ':' closed loops, ')+span(answer.outline.bounds));
    if(answer.drill){
      const d=answer.drill,holes=d.groups.map(g=>g.holes+' × '+text(g.tool_diameter)+' mm').join(', ');
      lines.push('Drill file: '+formatText(d.interpretation)+'. Holes: '+holes+(d.slots.length?'; '+d.slots.length+(d.slots.length===1?' slot':' slots'):''));
    }
    lines.push('Placed '+(answer.mirror_x==null?'':'mirrored about X'+text(answer.mirror_x)+', ')+'with offset X'+text(answer.offset[0])+' Y'+text(answer.offset[1])+
      (answer.bounds?'; it spans X'+text(answer.bounds[0])+' to X'+text(answer.bounds[2])+' and Y'+text(answer.bounds[1])+' to Y'+text(answer.bounds[3]):''));
    return lines;
  }
  // Every warning of the answer, each with the file it is about.
  function warnings(answer){
    const out=[];
    for(const [key,label] of [['copper','Copper'],['outline','Outline'],['drill','Drill file']])for(const w of answer[key]?.warnings||[])out.push(label+': '+w);
    return out.concat(answer.warnings||[]);
  }
  // The layers, objects and tools a reviewed board becomes. Each stage is a layer with one tool, numbered from
  // s.firstTool in cutting order: isolation, each drill diameter, slots, cut-out. Layers are returned in the
  // shape interchange.js stages an import in: objects name their layer by sourceLayer.
  function plan(answer,s){
    const side=answer.side==='bottom'?'underside':'top',prefix='PCB '+side+' · ',layers=[],objects=[],router={mode:'router',router_entry:'plunge'};
    let tool=s.firstTool||1;
    const stage=(key,name,color)=>{layers.push({key,name:prefix+name,color});};
    if(answer.copper){
      const c=answer.copper,v=s.kind==='v';
      number(s.depth,'Engraving depth',0.01,10);
      const settings={...router,operation:'line',tool_shape:v?'v':'flat',tool_diameter:c.tool_width,depth:s.depth,step:s.depth,tool_id:tool++,
        tool_name:v?'V-bit '+text(s.angle)+'° cutting '+text(c.tool_width)+' mm':'Engraver '+text(c.tool_width)+' mm',...(v?{v_angle:s.angle}:{})};
      stage('isolation','isolation','#b0562a');
      c.operations.forEach((operation,i)=>objects.push({name:'Isolation contours'+(c.operations.length>1?' '+(i+1):''),paths:operation.paths,sourceLayer:'isolation',operationSettings:{...settings}}));
    }
    if(answer.drill){
      number(s.drillDepth,'Drill depth',0.01,50);
      for(const group of answer.drill.groups){
        const d=group.tool_diameter,key='drill-'+text(d);
        const holes=answer.drill.operations.filter(o=>o.tool_diameter===d).flatMap(o=>o.paths).map(path=>holeCircle(path[0][0],path[0][1],d));
        stage(key,'drill '+text(d)+' mm','#1f7a4d');
        objects.push({name:'Drill '+text(d)+' mm · '+holes.length+(holes.length===1?' hole':' holes'),paths:holes,sourceLayer:key,
          operationSettings:{...router,operation:'drill',tool_shape:'drill',tool_diameter:d,depth:s.drillDepth,drill_peck:s.peck||0,drill_point_angle:s.pointAngle||118,tool_id:tool++,tool_name:'Drill '+text(d)+' mm'}});
      }
      // A slot is cut along its centre line with a cutter of its own width.
      const widths=[...new Set(answer.drill.slots.map(slot=>slot.diameter))].sort((a,b)=>a-b);
      for(const d of widths){
        const key='slots-'+text(d),paths=answer.drill.slots.filter(slot=>slot.diameter===d).map(slot=>slot.points);
        stage(key,'slots '+text(d)+' mm','#6a4fa3');
        objects.push({name:'Slots '+text(d)+' mm · '+paths.length,paths,sourceLayer:key,
          operationSettings:{...router,operation:'line',tool_shape:'flat',tool_diameter:d,depth:s.drillDepth,step:Math.min(s.cutStep||s.drillDepth,s.drillDepth),tool_id:tool++,tool_name:'Slot cutter '+text(d)+' mm'}});
      }
    }
    if(answer.outline){
      number(s.cutter,'Cut-out cutter',0.01,100);number(s.cutDepth,'Cut-out depth',0.01,50);number(s.cutStep,'Cut-out step down',0.01,10);
      stage('cut-out','cut-out','#34495e');
      // Outside the line, so that the board keeps its size; a loop inside the board is cut on its inner side.
      objects.push({name:'Board outline',paths:answer.outline.paths,sourceLayer:'cut-out',
        operationSettings:{...router,operation:'outside',tool_shape:'flat',tool_diameter:s.cutter,depth:s.cutDepth,step:Math.min(s.cutStep,s.cutDepth),tool_id:tool++,tool_name:'End mill '+text(s.cutter)+' mm'}});
    }
    const paths=objects.flatMap(o=>o.paths);
    return {layers,objects,tools:tool-(s.firstTool||1),paths:paths.length,points:paths.reduce((n,p)=>n+p.length,0)};
  }
  // What the project keeps of an imported board: where it was placed, so that the other side registers, and the
  // places the cutter cannot reach, so that they stay marked.
  function record(answer,layerIds){
    return {side:answer.side,offset:answer.offset,mirror_x:answer.mirror_x,outline_bounds:answer.outline?answer.outline.bounds:null,
      tool_width:answer.copper?answer.copper.tool_width:null,layers:layerIds,
      unreachable:(answer.copper?answer.copper.unreachable:[]).slice(0,200).map(place=>({at:place.at,gap:place.gap}))};
  }
  const api={vBitWidth,holeCircle,request,formatText,formatGuessed,summary,warnings,plan,record};
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  root.KerfKitePcb=api;
})(globalThis);
