/* Laser intensities are normalized. Each value drives the segment ending there. */
((root) => {
  'use strict';
  const clamp = v => Math.max(0, Math.min(1, v));
  function limit(n, label, min, max, integer=false) {
    if (!Number.isFinite(n) || n<min || n>max || (integer && !Number.isInteger(n))) throw Error(label+' must be '+(integer?'an integer ':'')+'between '+min+' and '+max);
    return n;
  }
  function sourceRect(width,height,crop) {
    const edges = ['left','top','right','bottom'].map(k=>limit(crop[k],'Crop '+k,0,99.99)/100);
    const [l,t,r,b]=edges;
    if(l+r>=1 || t+b>=1)throw Error('Leave a positive image width and height after cropping.');
    return [l*width,t*height,(1-l-r)*width,(1-t-b)*height];
  }
  function scanSize({width,height,interval,overscan}) {
    limit(width,'Width',.001,2000);limit(height,'Height',.001,2000);limit(interval,'Interval',.05,10);limit(overscan,'Overscan',0,50);
    const cols=Math.ceil(width/interval), rows=Math.ceil(height/interval);
    if(cols*rows>60000 || rows>2500)throw Error('Increase the interval or reduce the size to stay within 60,000 pixels and 2,500 scanlines.');
    return [cols,rows];
  }
  function maskContours(paths) {
    if(!Array.isArray(paths)||!paths.length)throw Error('Select a closed artwork outline for the mask.');
    let count=0;
    for(const p of paths){
      if(p.length<4||Math.hypot(p[0][0]-p.at(-1)[0],p[0][1]-p.at(-1)[1])>1e-6)throw Error('Close every contour in the selected mask.');
      count+=p.length;
    }
    return count;
  }
  function powerMap(pixels,options) {
    const {width,height,x,y,brightness,contrast,gamma,threshold,style,mask,invert}=options;
    const [cols,rows]=scanSize(options);
    if(pixels.length!==cols*rows*4)throw Error('Image samples do not match the scan resolution.');
    limit(brightness,'Brightness',-100,100);limit(contrast,'Contrast',-100,100);limit(gamma,'Gamma',.1,5);limit(threshold,'Threshold',0,100);
    if(!['gray','threshold','dither'].includes(style)||!['none','ellipse','selection'].includes(mask))throw Error('Unknown raster mode or mask.');
    const contours=mask==='selection'?options.contours:[];
    if(mask==='selection' && maskContours(contours)*rows>3000000)throw Error('Reduce mask complexity or scan resolution.');
    const allowed=new Uint8Array(rows*cols), values=new Float64Array(rows*cols);
    for(let j=0;j<rows;j++){
      const v=(j+.5)/rows, cy=y+height*(1-v), crossings=[];
      for(const path of contours)for(let i=1;i<path.length;i++){
        const [ax,ay]=path[i-1],[bx,by]=path[i];
        if((ay>cy)!==(by>cy))crossings.push(ax+(cy-ay)*(bx-ax)/(by-ay));
      }
      crossings.sort((a,b)=>a-b);
      let cross=0;
      for(let i=0;i<cols;i++){
        const index=j*cols+i, k=index*4, u=(i+.5)/cols;
        while(cross<crossings.length && crossings[cross]<=x+width*u)++cross;
        if(!pixels[k+3] || (mask==='ellipse' && (2*u-1)**2+(2*v-1)**2>1) || (mask==='selection' && cross%2===0))continue;
        allowed[index]=1;
        const luminance=(.2126*pixels[k]+.7152*pixels[k+1]+.0722*pixels[k+2])/255;
        const gain=contrast===100?1e6:(100+contrast)/(100-contrast);
        const light=Math.pow(clamp((luminance-.5)*gain+.5+brightness/100),1/gamma);
        values[index]=(invert?light:1-light)*pixels[k+3]/255;
      }
    }
    if(style!=='gray'){
      const diffuse=(i,j,error)=>{const k=j*cols+i;if(i>=0&&i<cols&&j<rows&&allowed[k])values[k]+=error;};
      for(let j=0;j<rows;j++)for(let i=0;i<cols;i++){
        const k=j*cols+i;if(!allowed[k])continue;
        const previous=values[k];
        // Compare luminance with the threshold, retaining the original equality-to-white rule.
        values[k]=previous>1-threshold/100?1:0;
        if(style==='dither'){
          const err=previous-values[k];diffuse(i+1,j,err*7/16);diffuse(i-1,j+1,err*3/16);diffuse(i,j+1,err*5/16);diffuse(i+1,j+1,err/16);
        }
      }
    }
    return {cols,rows,values};
  }
  function scanPaths({cols,rows,values}, {x,y,width,height,overscan}) {
    const paths=[], powers=[];
    for(let row=0;row<rows;row++){
      const py=y+height*(1-(row+.5)/rows),reverse=row%2===1;
      const from=reverse?x+width:x, to=reverse?x:x+width, direction=reverse?-1:1;
      const points=[[from,py]],s=[0];
      if(overscan){points.unshift([from-direction*overscan,py]);s.unshift(0);}
      for(let i=0;i<cols;i++){points.push([from+direction*(i+1)*width/cols,py]);s.push(values[row*cols+(reverse?cols-1-i:i)]);}
      if(overscan){points.push([to+direction*overscan,py]);s.push(0);}
      paths.push(points);powers.push(s);
    }
    return {paths,powers};
  }
  function materialGrid({x,y,rows,cols,size,gap,feedMin,feedMax,powerMin,powerMax,maxS,pattern,interval,bedWidth,bedHeight}) {
    limit(rows,'Feed steps',2,10,true);limit(cols,'Power steps',2,10,true);limit(size,'Cell size',1,100);limit(gap,'Gap',.1,100);
    limit(feedMin,'Minimum feed',1,10000);limit(feedMax,'Maximum feed',feedMin,10000);
    limit(powerMin,'Minimum power',.01,100);limit(powerMax,'Maximum power',powerMin,100);limit(maxS,'Controller power scale',1,100000,true);
    if(feedMin===feedMax || powerMin===powerMax)throw Error('Choose a range for both feed and power.');
    limit(x,'Grid X',0,bedWidth);limit(y,'Grid Y',0,bedHeight);
    if(x+cols*size+(cols-1)*gap>bedWidth || y+rows*size+(rows-1)*gap>bedHeight)throw Error('Material grid exceeds the work area.');
    if(pattern!=='outline' && pattern!=='hatch')throw Error('Choose an outline or hatch sample.');
    let lines=1;
    if(pattern==='hatch'){
      limit(interval,'Hatch interval',.05,10);
      lines=Math.ceil(size/interval);
      if(lines*rows*cols>2500)throw Error('Choose a larger hatch interval or a smaller grid.');
    }
    const cells=[];
    for(let row=0;row<rows;row++)for(let col=0;col<cols;col++){
      const left=x+col*(size+gap), bottom=y+row*(size+gap);
      const feed=Math.round(10*(feedMin+(feedMax-feedMin)*row/(rows-1)))/10;
      const percent=powerMin+(powerMax-powerMin)*col/(cols-1), power=Math.round(maxS*percent/100);
      if(power<1)throw Error('The lowest power rounds to zero on this controller scale.');
      const paths=[];
      if(pattern==='outline')paths.push([[left,bottom],[left+size,bottom],[left+size,bottom+size],[left,bottom+size],[left,bottom]]);
      else for(let i=0;i<lines;i++){
        const py=bottom+(i+.5)*size/lines;
        paths.push(i%2?[[left+size,py],[left,py]]:[[left,py],[left+size,py]]);
      }
      cells.push({name:'Material test · '+feed+' mm/min · '+percent.toFixed(1)+'% (S'+power+')', paths,
        operationSettings:{mode:'laser',operation:'line',feed,power,passes:1,lead_in:0,lead_out:0,ramp_in:0,ramp_out:0,start_power:100,end_power:100}});
    }
    return cells;
  }
  const api={scanSize,sourceRect,powerMap,scanPaths,materialGrid};
  root.CNCLaserWorkshop=api;
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
})(globalThis);
