/* A circular outline carries the hole centre through placement, project IO and CAM. */
(() => {
  'use strict';
  const val=id=>$(id).valueAsNumber;
  $('addDrillHoles').onclick=task(()=>{
    if($('mode').value!=='router') throw Error('Choose router mode to drill holes.');
    const x=val('drillX'),y=val('drillY'),columns=val('drillCols'),rows=val('drillRows');
    const dx=val('drillDX'),dy=val('drillDY'),diameter=val('tool_diameter'),r=diameter/2;
    if(![x,y,diameter].every(Number.isFinite) || diameter<.01 || diameter>100)
      throw Error('Enter a first hole centre and cutter diameter (0.01–100 mm).');
    if(![columns,rows].every(n=>Number.isInteger(n)&&n>=1&&n<=30)) throw Error('Enter 1–30 rows and columns.');
    if((columns>1&&(!Number.isFinite(dx)||dx<diameter)) || (rows>1&&(!Number.isFinite(dy)||dy<diameter)))
      throw Error('For multiple holes, set the pitch to at least the cutter diameter.');
    const paths=[];
    for(let row=0;row<rows;row++) for(let col=0;col<columns;col++){
      const cx=x+(col?col*dx:0),cy=y+(row?row*dy:0);
      const circle=Array.from({length:32},(_,i)=>[cx+r*Math.cos(i*Math.PI/16),cy+r*Math.sin(i*Math.PI/16)]);
      paths.push([...circle,circle[0].slice()]);
    }
    const settings={mode:'router',operation:'drill',router_entry:'plunge',tool_shape:'drill'};
    for(const key of ['tool_diameter','depth','safe','plunge','power','drill_peck','drill_point_angle']){
      settings[key]=val(key);
      if(!Number.isFinite(settings[key])) throw Error('Enter the drill settings before adding holes.');
    }
    addGenerated('Drilling · '+columns+' × '+rows,paths,{operationSettings:settings});
    message(paths.length+' drill location'+(paths.length===1?'':'s')+' added. Generate a preview to inspect the retracts and plunges.');
  });
})();
