/* Original procedural artwork; no external artwork or fonts. */
(function(root){
'use strict';
const ornaments=typeof module!=='undefined'?require('./ornaments.js'):root.CNCOrnaments;
const TAU=Math.PI*2, close=p=>[...p,[...p[0]]];
function circle(cx,cy,r,n=48){return close(Array.from({length:n},(_,i)=>[cx+r*Math.cos(i*TAU/n),cy+r*Math.sin(i*TAU/n)]));}
function flower(petals,pointed=false){const outline=close(Array.from({length:petals*24},(_,i)=>{const a=i*TAU/(petals*24),wave=(1+Math.cos(petals*a))/2,r=20+26*(pointed?wave*wave:Math.sqrt(wave));return[50+r*Math.cos(a),50+r*Math.sin(a)];}));return[circle(50,50,8),outline];}
function leaf(cx,cy,length,width,angle){const points=[];for(let i=0;i<=40;i++){const t=i/40;points.push([length*t,width*Math.sin(Math.PI*t)]);}for(let i=39;i>=0;i--){const t=i/40;points.push([length*t,-width*Math.sin(Math.PI*t)]);}const c=Math.cos(angle),s=Math.sin(angle);return close(points.map(([x,y])=>[cx+x*c-y*s,cy+x*s+y*c]));}
function vine(count,curved=false){const paths=[],stem=[];for(let i=0;i<=80;i++){const t=i/80;stem.push([50+(curved?12*Math.sin(t*TAU):0),5+90*t]);}paths.push(stem);for(let i=1;i<=count;i++){const t=i/(count+1),x=50+(curved?12*Math.sin(t*TAU):0),y=5+90*t;paths.push(leaf(x,y,25,6,i%2?Math.PI*.22:Math.PI*.78));}return paths.slice(1).reduce((result,mask)=>ornaments.clip(result,mask),[stem]).concat(paths.slice(1));}
function star(points,inner=.45){return[close(Array.from({length:points*2},(_,i)=>{const a=Math.PI/2+i*Math.PI/points,r=i%2?46*inner:46;return[50+r*Math.cos(a),50+r*Math.sin(a)];}))];}
function heart(){return[close(Array.from({length:120},(_,i)=>{const a=i*TAU/120;return[50+2.7*16*Math.sin(a)**3,48+2.7*(13*Math.cos(a)-5*Math.cos(2*a)-2*Math.cos(3*a)-Math.cos(4*a))];}))];}
function rosette(petals,rings){const paths=[];for(let j=0;j<rings;j++)paths.push(...flower(petals,j%2===1).slice(1).map(p=>p.map(([x,y])=>[50+(x-50)*(1-j*.18),50+(y-50)*(1-j*.18)])));paths.unshift(circle(50,50,4));return paths;}
function badge(points){return[close(Array.from({length:points},(_,i)=>[50+45*Math.cos(i*TAU/points),50+45*Math.sin(i*TAU/points)]))];}
function normalize(paths){let minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;for(const path of paths)for(const[x,y]of path){minX=Math.min(minX,x);minY=Math.min(minY,y);maxX=Math.max(maxX,x);maxY=Math.max(maxY,y);}return paths.map(path=>path.map(([x,y])=>[(x-minX)/(maxX-minX||1)*100,(y-minY)/(maxY-minY||1)*100]));}
// Register recipes first. Their geometry is generated only when a matching entry is used.
const recipes = new Map();
let output;
function register(name, build) {
    if (recipes.has(name)) throw new Error('Duplicate template recipe: ' + name);
    recipes.set(name, build);
}
function add(id, name, category, paths) {
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (const path of paths) {
        for (const [x, y] of path) {
            minX = Math.min(minX, x);
            maxX = Math.max(maxX, x);
            minY = Math.min(minY, y);
            maxY = Math.max(maxY, y);
        }
    }
    const aspect = (maxX - minX) / (maxY - minY || 1);
    output.push({id, name, category, paths: normalize(paths), aspect, builtin: true});
}

register('artisan', () => {
for(const d of ornaments.designs)add(d.id,d.name,d.category,d.paths);
});
register('classic', () => {
for(const n of [4,5,6,7,8,10,12,16]){add('flower-round-'+n,`${n}-petal rounded flower`,'Flowers',flower(n));add('flower-point-'+n,`${n}-petal pointed flower`,'Flowers',flower(n,true));}
for(const n of [3,4,5,6,7,8]){add('vine-straight-'+n,`Upright vine · ${n} leaves`,'Vines',vine(n));add('vine-wave-'+n,`Winding vine · ${n} leaves`,'Vines',vine(n,true));}
for(const n of [6,8,10,12])add('rosette-'+n,`${n}-fold layered rosette`,'Flowers',rosette(n,3));
for(const ratio of [3,5,8,12]){add('leaf-'+ratio,`Leaf · width ${ratio}`,'Leaves',[leaf(5,50,90,ratio,0),[[5,50],[95,50]]]);add('sprig-'+ratio,`Leaf pair · width ${ratio}`,'Leaves',[leaf(50,10,70,ratio,Math.PI*.35),leaf(50,10,70,ratio,Math.PI*.65),[[50,0],[50,10]]]);}
for(const n of [4,5,6,8,10,12])add('star-'+n,`${n}-point star`,'Symbols',star(n));
add('heart','Heart','Symbols',heart());
add('double-heart','Nested hearts','Symbols',[...heart().map(p=>p.map(([x,y])=>[50+(x-50)*.65,50+(y-50)*.65])),...heart()]);
add('arrow','Direction arrow','Symbols',[[[5,35],[60,35],[60,10],[95,50],[60,90],[60,65],[5,65],[5,35]]]);
add('lightning','Lightning bolt','Symbols',[[[60,100],[10,40],[45,40],[30,0],[90,60],[55,60],[60,100]]]);
add('cross','Cross','Symbols',[[[35,0],[65,0],[65,35],[100,35],[100,65],[65,65],[65,100],[35,100],[35,65],[0,65],[0,35],[35,35],[35,0]]]);
for(const n of [3,4,5,6,8])add('badge-'+n,`${n}-sided badge`,'Tags & badges',badge(n));
add('round-tag','Round tag with hanging hole','Tags & badges',[circle(50,83,4),circle(50,50,45)]);
add('rect-tag','Clipped gift tag','Tags & badges',[circle(50,84,4),[[10,0],[90,0],[90,75],[65,100],[35,100],[10,75],[10,0]]]);
add('key-tag','Key tag','Tags & badges',[circle(15,50,4),[[0,25],[80,25],[100,50],[80,75],[0,75],[0,25]]]);
add('ring','Circular frame','Frames',[circle(50,50,37),circle(50,50,48)]);
add('rect-frame','Rectangular frame','Frames',[[[12,12],[88,12],[88,88],[12,88],[12,12]],[[0,0],[100,0],[100,100],[0,100],[0,0]]]);
add('diamond-frame','Diamond frame','Frames',[...badge(4).map(p=>p.map(([x,y])=>[50+(x-50)*.7,50+(y-50)*.7])),...badge(4)]);
add('sunburst','Sunburst','Symbols',[circle(50,50,16),...star(16,.6)]);
});

// Expanded original engraving collection. Geometry is generated deterministically.
// These are line-art compositions; closed outlines may be profiled separately.
const transform=(paths,x=0,y=0,a=0,k=1)=>paths.map(p=>p.map(([px,py])=>[x+k*(px*Math.cos(a)-py*Math.sin(a)),y+k*(px*Math.sin(a)+py*Math.cos(a))]));
const arc=(x,y,r,a,b,n=48)=>Array.from({length:n+1},(_,i)=>[x+r*Math.cos(a+(b-a)*i/n),y+r*Math.sin(a+(b-a)*i/n)]);
const cubic=(a,b,c,d,n=32)=>Array.from({length:n+1},(_,i)=>{const t=i/n,u=1-t;return [0,1].map(k=>u*u*u*a[k]+3*u*u*t*b[k]+3*u*t*t*c[k]+t*t*t*d[k]);});
function overlay(paths,shape){return [...ornaments.clip(paths,shape[0]),...shape];}
function botanicalLeaf(len,width,type){const edge=close(Array.from({length:64},(_,i)=>{const a=i*TAU/64,t=(1-Math.cos(a))/2,scallop=type%3===0?1+.19*Math.sin(t*Math.PI*(5+type%4)):1;return[len*t,width*Math.sign(Math.sin(a))*Math.abs(Math.sin(a))**(1+type*.045)*scallop*(1+type*.015*(t-.5))];}));const p=[edge,[[0,0],[len*.96,0]]];for(const t of [.22,.4,.58,.76])for(const sign of [-1,1])p.push([[len*t,0],[len*(t+.12),sign*width*.7*Math.sin(t*Math.PI)]]);return p;}
const species=['Willow','Laurel','Eucalyptus','Rose','Fern','Hazel','Sage','Myrtle','Ash','Camellia','Bay','Birch'];
function branch(n){let p=[cubic([0,0],[28,14],[70,-12],[100,5],70)];const count=7+n%5;for(let i=0;i<count;i++){const t=(i+1)/(count+1),x=t*100,y=42*(1-t)**2*t-36*(1-t)*t*t+5*t**3,side=i%2?1:-1,a=side*(.55+(n%4)*.11);p=overlay(p,transform(botanicalLeaf(14+n%4,2.6+(n%5)*.65,n),x,y,a));if(n%3===1&&i%3===0){const berry=circle(x+3,y-side*7,2.1,20);p=overlay(p,[berry]);}}return p;}
function botanicalWreath(n){let p=[];for(const side of [-1,1]){p.push(arc(0,0,38,side===1?-1.42:1.72,side===1?1.42:4.56));for(let i=0;i<9;i++){const a=-1.3+i*.31,x=side*38*Math.cos(a),y=38*Math.sin(a);for(const b of [-.48,.48])p=overlay(p,transform(botanicalLeaf(10+n%4,2.1+n%3*.5,n),x,y,side===1?a+Math.PI/2+b:Math.PI/2-a+b));}}return p;}
for(let n=0;n<species.length;n++){
    register('branches-' + n, () => {
const name=species[n],b=branch(n),corner=[...transform(b,0,0,0,.75),...transform(b,0,0,Math.PI/2,.75)];add('collection-branch-'+n,name+' engraved branch','Botanical vines',b);add('collection-vine-'+n,name+' climbing double vine','Vines',[...b,...transform(b,100,26,Math.PI)]);add('collection-leaf-'+n,name+' veined leaf study','Leaves',botanicalLeaf(50,8+n%5,n));add('collection-corner-'+n,name+' botanical corner','Botanical corners',corner);add('collection-frame-'+n,name+' botanical invitation frame','Botanical frames',[...transform(corner,0,0,0,.4),...transform(corner,110,0,Math.PI/2,.4),...transform(corner,110,75,Math.PI,.4),...transform(corner,0,75,-Math.PI/2,.4),[[35,0],[75,0]],[[35,75],[75,75]],[[0,35],[0,40]],[[110,35],[110,40]]]);add('collection-wreath-'+n,name+' ceremonial wreath','Wreaths',botanicalWreath(n));
    });
}
function bloom(n){let p=[];const count=5+n;for(let ring=0;ring<2;ring++)for(let i=0;i<count;i++){const a=(i+ring*.5)*TAU/count;const petal=close([...cubic([7,0],[17,9+ring*2],[40-ring*7,12],[43-ring*9,0]),...cubic([43-ring*9,0],[34,-10],[17,-9],[7,0]).slice(1)]);p=overlay(p,transform([petal],0,0,a));}p=overlay(p,[circle(0,0,8,32)]);for(let i=0;i<9;i++)p.push(circle(4*Math.cos(i*TAU/9),4*Math.sin(i*TAU/9),.6,10));return p;}
const flowerNames=['Dahlia','Cosmos','Daisy','Aster','Zinnia','Gazania','Chrysanthemum','Gerbera','Anemone','Marigold','Coreopsis','Calendula'];
for(let n=0;n<12;n++){
    register('blossoms-' + n, () => {
const b=bloom(n);add('collection-bloom-'+n,flowerNames[n]+' layered bloom','Flowers',b);add('collection-stem-'+n,flowerNames[n]+' botanical stem','Flowers',[...transform(branch(n),0,-95,Math.PI/2,.8),...b]);let bouquet=[];for(const [x,y,a,k]of [[-26,0,-.25,.55],[26,0,.3,.55],[0,26,0,.65]]){bouquet.push(cubic([0,-65],[x,-40],[x,y-25],[x,y]));bouquet=overlay(bouquet,transform(b,x,y,a,k));}add('collection-bouquet-'+n,flowerNames[n]+' three-flower bouquet','Flowers',bouquet);
    });
}
function flourish(n){const p=[],turn=1.05+n%4*.14;for(const side of [-1,1]){const spiral=Array.from({length:95},(_,i)=>{const t=i/94,a=t*TAU*turn,r=13*(1-t)+.25;return[side*(29+r*Math.cos(a)),6+r*Math.sin(a)];});p.push(cubic([0,0],[side*12,-19],[side*38,-13],spiral[0]),spiral);for(let i=0;i<3+n%3;i++)p.push(...transform([botanicalLeaf(13-i,3.5,n)[0]],side*(i*6),2+i*2,side===1?.6+i*.16:Math.PI-.6-i*.16));}if(n%2)p.push(...transform([botanicalLeaf(20,4,n)[0]],0,0,Math.PI/2));if(n>=8)for(let i=0;i<7;i++)p.push(circle((i-3)*5,-14-Math.sin(i/6*Math.PI)*3,.6+(i%2)*.25,12));return p;}
for(let n=0;n<16;n++){
    register('flourishes-' + n, () => {
const f=flourish(n);add('collection-scroll-'+n,['Regency','Rococo','Palmette','Baroque'][n%4]+' scroll '+(1+Math.floor(n/4)),'Ornamental swirls',f);add('collection-cartouche-'+n,['Oval','Cameo','Medallion','Crest'][n%4]+' ornamental frame '+(1+Math.floor(n/4)),'Frames',[arc(0,0,40,.3,Math.PI-.3).map(([x,y])=>[x*1.6,y]),arc(0,0,40,Math.PI+.3,TAU-.3).map(([x,y])=>[x*1.6,y]),...transform(f,0,40,0,.65),...transform(f,0,-40,Math.PI,.65)]);
    });
}
for(let n=0;n<24;n++){
    register('borders-' + n, () => {
let p=[];const repeats=3+n%4;for(let i=0;i<repeats;i++){const motif=n<8?flourish(n):n<16?branch(n-8):bloom(n-16);p.push(...transform(motif,i*65,0,0,n<8?.55:n<16?.55:.55));}p.push([[-30,-34],[repeats*65-25,-34]],[[-30,34],[repeats*65-25,34]]);add('collection-border-'+n,['Scroll','Botanical','Floral'][Math.floor(n/8)]+' repeating border '+(n%8+1),'Decorative borders',p);
    });
}
for(let n=0;n<16;n++){
    register('mandalas-' + n, () => {
let p=[circle(0,0,7,40),circle(0,0,46,96)];const count=6+n;for(let i=0;i<count;i++){const a=i*TAU/count;p.push(...transform([botanicalLeaf(30,3+n%4,n)[0]],10*Math.cos(a),10*Math.sin(a),a));p.push(arc(0,0,38,a+.03,a+TAU/count-.03,12));}add('collection-mandala-'+n,`${count}-fold lotus mandala`,'Mandalas',p);
    });
}
for(let n=0;n<12;n++){
    register('seasonal-' + n, () => {
const p=[];for(let i=0;i<6;i++){const ray=[[[0,0],[0,44]]];for(let j=0;j<3+n%3;j++){const y=12+j*5.5,w=10-j*.9+(n%4);ray.push([[-w,y+7],[0,y],[w,y+7]]);}if(n>=6)ray.push(...transform([botanicalLeaf(8,3,n)[0]],0,36,Math.PI/2));p.push(...transform(ray,0,0,i*TAU/6));}p.push(circle(0,0,5+n%4,24));add('collection-snow-'+n,'Crystal snowflake '+(n+1),'Seasonal',p);
    });
}
for(let n=0;n<16;n++){
    register('labels-' + n, () => {
const w=65+n%4*8,h=35+Math.floor(n/4)*5,r=7;const outline=close([...arc(w/2-r,h/2-r,r,0,Math.PI/2,12),...arc(-w/2+r,h/2-r,r,Math.PI/2,Math.PI,12),...arc(-w/2+r,-h/2+r,r,Math.PI,Math.PI*1.5,12),...arc(w/2-r,-h/2+r,r,Math.PI*1.5,TAU,12)]);const p=[circle(-w/2+7,0,2.2,24),outline,...transform(flourish(n),3,-h*.26,0,.28)];add('collection-label-'+n,['Gift','Pantry','Luggage','Garden'][Math.floor(n/4)]+' engraved label '+(n%4+1),'Tags & badges',p);
    });
}
for(let n=0;n<12;n++){
    register('butterflies-' + n, () => {
let p=[];for(const side of [-1,1]){const wing=close([...cubic([0,3],[side*45,60],[side*(54+n%4*3),24],[side*14,-8]),...cubic([side*14,-8],[side*48,-33],[side*12,-47],[0,-14]).slice(1)]);p.push(wing);for(let i=0;i<3;i++)p.push(cubic([side*3,0],[side*10,7+i*7],[side*20,15+i*7],[side*(29+i*4),17+i*5]));p.push(...transform([botanicalLeaf(16+n%3,5,n)[0]],side*12,-16,side===1?-.65:Math.PI+.65));p.push(cubic([0,8],[side*2,18],[side*7,21],[side*9,17]));}p=overlay(p,[[...arc(0,-2,3,0,TAU,32)].map(([x,y])=>[x,y*4])]);for(let i=0;i<n%4+2;i++)for(const side of [-1,1])p.push(circle(side*(21+i*5),19+i*3,1.2,16));add('collection-butterfly-'+n,['Swallowtail','Meadow','Lacewing','Garden'][n%4]+' butterfly study '+(1+Math.floor(n/4)),'Butterflies',p);
    });
}
for(let n=0;n<12;n++){
    register('landscapes-' + n, () => {
let p=[circle(0,0,48,96)];const mountains=[[[-43,-10],[-22,20+n%4*3],[-2,-10]],[[-14,-10],[13,31-n%3*3],[42,-10]]];p.push(...mountains,[[ -28,11],[-22,7],[-16,11]],[[6,23],[13,17],[20,23]],circle(-17,29,5+n%3,32));for(let i=0;i<3;i++)p.push(cubic([-36+i*3,-20-i*6],[-10,-12-i*6],[12,-28-i*5],[36-i*3,-18-i*7]));for(let i=0;i<2+n%3;i++){const x=-33+i*17,y=-10;p.push([[x,y],[x,y+10+i%2*4]],[[x-4,y+3],[x,y+10+i%2*4],[x+4,y+3]]);}add('collection-landscape-'+n,['Alpine lake','Mountain river','Forest ridge'][n%3]+' landscape badge '+(1+Math.floor(n/3)),'Nature & landscapes',p);
    });
}
for(let n=0;n<16;n++){
    register('patterns-' + n, () => {
const p=[];for(let row=0;row<4;row++)for(let col=0;col<4;col++){const x=col*24,y=row*24;if(n<4)p.push(...transform([close([[0,10],[10,0],[0,-10],[-10,0]])],x,y,n*Math.PI/12));else if(n<8)p.push(...transform([circle(0,0,9,4+n)],x,y,(row+col)*Math.PI/8));else if(n<12)p.push(arc(x,y,10,(row+col)%2*Math.PI,((row+col)%2+1)*Math.PI,24),arc(x,y,6,(row+col)%2*Math.PI,((row+col)%2+1)*Math.PI,24),[[x-10,y-12],[x+10,y-12-n%4]]);else p.push(...transform([botanicalLeaf(19,4+(n%4)*.6,n)[0]],x-8,y-6,(row+col)%2*Math.PI/2));}add('collection-pattern-'+n,['Diamond lattice','Polygon tessellation','Scallop repeat','Leaf parquet'][Math.floor(n/4)]+' panel '+(n%4+1),'Geometric patterns',p);
    });
}
for(let n=0;n<16;n++){
    register('symbols-' + n, () => {
const count=5+n,inner=.35+n%4*.08;const outer=star(count,inner).map(p=>p.map(([x,y])=>[x-50,y-50]));const p=[...outer,circle(0,0,9,40)];for(let i=0;i<count;i++){const a=Math.PI/2+i*TAU/count;p.push([[12*Math.cos(a),12*Math.sin(a)],[35*Math.cos(a),35*Math.sin(a)]]);}add('collection-compass-'+n,`${count}-ray engraved compass medallion`,'Symbols',p);
    });
}


// Purpose-based motif library: individually constructed original vector drawings.
const poly=points=>close(points),rect=(x,y,w,h)=>poly([[x,y],[x+w,y],[x+w,y+h],[x,y+h]]);
const sail=[poly([[-30,-12],[30,-12],[18,-25],[-18,-25]]),[[0,-12],[0,36]],poly([[3,32],[27,-7],[3,-7]]),poly([[-3,22],[-24,-7],[-3,-7]])];
register('daily-life', () => {
const icons={};
function icon(category,name,paths){(icons[category]??=[]).push({name,paths});add('purpose-'+category.toLowerCase().replace(/[^a-z]+/g,'-')+'-'+name.toLowerCase().replace(/[^a-z]+/g,'-'),name,category,paths);}

icon('Nautical','Coastal sailboat',sail);
icon('Nautical','Admiralty anchor',[circle(0,27,6),[[0,21],[0,-24]],[[-17,11],[17,11]],cubic([-27,-8],[-25,-39],[25,-39],[27,-8]),[[-27,-8],[-31,-17],[-21,-14]],[[27,-8],[31,-17],[21,-14]]]);
icon('Nautical','Ship wheel',[circle(0,0,23),circle(0,0,18),circle(0,0,5),...Array.from({length:8},(_,i)=>transform([[[5,0],[35,0]],circle(33,0,2)],0,0,i*TAU/8)).flat()]);
icon('Nautical','Scallop seashell',[poly([...arc(0,0,32,0,Math.PI),[0,-12]]),...Array.from({length:7},(_,i)=>[[0,-10],[30*Math.cos((i+1)*Math.PI/8),30*Math.sin((i+1)*Math.PI/8)]])]);
icon('Nautical','Lighthouse beacon',[poly([[-16,-32],[16,-32],[11,21],[-11,21]]),rect(-14,21,28,10),poly([[-17,31],[0,41],[17,31]]),rect(-3,-32,6,12),[[-13,-8],[13,-8]],[[-12,8],[12,8]],[[-22,27],[-39,35]],[[22,27],[39,35]],[[-22,23],[-40,18]],[[22,23],[40,18]]]);
icon('Nautical','Rolling ocean waves',[...Array.from({length:3},(_,i)=>cubic([-40,-i*12],[-17,24-i*12],[10,-24-i*12],[40,-i*12]))]);
icon('Nautical','Nautilus shell',[Array.from({length:200},(_,i)=>{const a=i/199*TAU*2.7,r=2+i/199*34;return [r*Math.cos(a),r*Math.sin(a)];})]);
icon('Nautical','Mariner compass',[circle(0,0,36),poly([[0,32],[7,7],[32,0],[7,-7],[0,-32],[-7,-7],[-32,0],[-7,7]]),circle(0,0,4),...Array.from({length:8},(_,i)=>transform([[[30,0],[34,0]]],0,0,i*TAU/8)).flat()]);
icon('Space & astronomy','Ringed planet',[...ornaments.clip([circle(0,0,23)],poly([[-38,-8],[38,4],[38,9],[-38,-3]])),...Array.from({length:2},(_,i)=>close(Array.from({length:100},(_,j)=>{const a=j*TAU/100;return [40*Math.cos(a),7*Math.sin(a)+.18*40*Math.cos(a)+i*3];})))]);
icon('Space & astronomy','Retro rocket',[poly([[-11,-20],[-11,12],[0,35],[11,12],[11,-20]]),circle(0,10,5),poly([[-11,-5],[-23,-23],[-11,-18]]),poly([[11,-5],[23,-23],[11,-18]]),poly([[-7,-23],[0,-40],[7,-23]])]);
icon('Space & astronomy','Crescent moon',[close([...arc(0,0,30,Math.PI/3,Math.PI*5/3,70),...cubic([15,-26],[-8,-14],[-8,14],[15,26],60)])]);
icon('Space & astronomy','Solar system orbits',[circle(0,0,8),...Array.from({length:4},(_,i)=>circle(0,0,17+i*9,80)),...Array.from({length:4},(_,i)=>circle((17+i*9)*Math.cos(i*1.6),(17+i*9)*Math.sin(i*1.6),2+i*.5,24))]);
icon('Space & astronomy','Shooting star',[...transform(star(5).map(p=>p.map(([x,y])=>[x-50,y-50])),22,10,0,.36),cubic([-40,-20],[-18,-20],[-5,0],[10,7]),cubic([-40,-30],[-15,-25],[0,-10],[10,0]),cubic([-34,-9],[-12,-8],[0,9],[10,13])]);
icon('Space & astronomy','Lunar phases',[...Array.from({length:5},(_,i)=>[circle(i*24,0,9),...(i===2?[]:[arc(i*24,0,9,-Math.PI/2,Math.PI/2).map(([x,y])=>[i*24+(x-i*24)*(i-2)/2,y])])]).flat()]);
icon('Space & astronomy','Constellation study',[[[-30,5],[-16,18],[0,9],[18,22],[32,9],[11,-10],[-13,-14]],...[[ -30,5],[-16,18],[0,9],[18,22],[32,9],[11,-10],[-13,-14]].map(([x,y])=>circle(x,y,2,16))]);
icon('Space & astronomy','Observatory telescope',[poly([[-30,18],[21,38],[27,23],[-24,3]]),[[0,8],[0,-5],[-23,-32]],[[0,-5],[23,-32]],[[0,-5],[0,-32]],[[25,29],[34,32]]]);
const note=[circle(-7,-17,6).map(([x,y])=>[x,y*.7]),[[-1,-12],[-1,29],[20,20],[20,11],[-1,20]]];
icon('Music','Quaver note',note);
icon('Music','Beamed eighth notes',[...note,circle(20,-10,6).map(([x,y])=>[x,y*.7]),[[26,-7],[26,27],[-1,34],[-1,29]]]);
icon('Music','Acoustic guitar',[close([...cubic([-5,5],[-25,20],[-29,-6],[-17,-12]),...cubic([-17,-12],[-41,-35],[35,-42],[19,-14]).slice(1),...cubic([19,-14],[30,6],[14,16],[5,5]).slice(1)]),rect(-4,5,8,39),rect(-6,44,12,10),circle(0,-12,6),[[-8,-28],[8,-28]],...[-2,0,2].map(x=>[[x,-28],[x,51]])]);
icon('Music','Piano keyboard',[rect(-35,-20,70,40),...Array.from({length:6},(_,i)=>[[-25+i*10,-20],[-25+i*10,20]]),...[-25,-15,5,15,25].map(x=>rect(x-2,0,4,20))]);
icon('Music','Studio headphones',[arc(0,0,28,0,Math.PI),arc(0,0,23,0,Math.PI),rect(-28,-17,8,17),rect(20,-17,8,17)]);
icon('Music','Vinyl record',[circle(0,0,35),circle(0,0,10),circle(0,0,2),...Array.from({length:5},(_,i)=>arc(0,0,16+i*3,.3,2.8)),...Array.from({length:5},(_,i)=>arc(0,0,16+i*3,3.4,5.9))]);
icon('Music','Concert microphone',[arc(0,10,10,0,Math.PI),[[-10,10],[-10,-6],[10,-6],[10,10]],cubic([-16,0],[-16,-27],[16,-27],[16,0]),[[0,-20],[0,-34]],[[-13,-34],[13,-34]],...[-1,4,9].map(y=>[[-7,y],[7,y]])]);
icon('Music','Audio waveform',[...Array.from({length:25},(_,i)=>{const h=3+17*Math.sin(i*.67)**2*Math.sin((i+1)/26*Math.PI);return [[i*3,-h],[i*3,h]];})]);
icon('Workshop & makers','Crossed spanners',[...transform([poly([[-4,-28],[4,-28],[4,10],[12,17],[11,30],[5,21],[-5,21],[-11,30],[-12,17],[-4,10]])],0,0,.6),...transform([poly([[-3,-28],[3,-28],[3,16],[10,16],[10,25],[-10,25],[-10,16],[-3,16]])],0,0,-.6)]);
icon('Workshop & makers','Claw hammer',[rect(-3,-28,6,43),poly([[-3,15],[-17,15],[-21,26],[-9,23],[7,23],[18,28],[18,14],[7,17],[3,15]])]);
icon('Workshop & makers','Carpenters square',[poly([[-30,-30],[30,-30],[30,-20],[-20,-20],[-20,30],[-30,30]]),...Array.from({length:8},(_,i)=>[[-30,-14+i*5],[-25,-14+i*5]]),...Array.from({length:8},(_,i)=>[[-14+i*5,-30],[-14+i*5,-25]])]);
icon('Workshop & makers','Hand saw',[poly([[-33,5],[22,15],[33,5],[25,-5],[18,-2],[12,-5],[6,-2],[0,-5],[-6,-2],[-12,-5],[-18,-2],[-24,-5]]),poly([[23,10],[29,5],[25,1],[21,3]])]);
icon('Workshop & makers','Hex nut',[circle(0,0,12),poly(Array.from({length:6},(_,i)=>[28*Math.cos(i*TAU/6),28*Math.sin(i*TAU/6)]))]);
icon('Workshop & makers','Bench clamp',[poly([[-20,-24],[-20,25],[20,25],[20,14],[-8,14],[-8,-13],[20,-13],[20,-24]]),[[13,-13],[13,9]],[[6,9],[20,9]],[[13,-24],[13,-35]],[[3,-35],[23,-35]]]);
icon('Workshop & makers','Digital caliper',[rect(-35,-3,70,6),poly([[-29,-3],[-29,-20],[-24,-20],[-24,-3]]),poly([[10,-3],[10,-20],[5,-20],[5,-3]]),rect(0,-5,16,13),rect(3,-1,10,6),...Array.from({length:8},(_,i)=>[[-22+i*7,0],[-22+i*7,3]])]);
icon('Workshop & makers','Sewing needle and thread',[poly([[-2,25],[0,-32],[2,25],[0,32]]),[[0,24],[0,29]],cubic([0,27],[45,38],[-34,-3],[28,-18])]);
icon('Food & kitchen','Coffee cup',[poly([[-22,17],[18,17],[14,-16],[-18,-16]]),cubic([18,11],[43,17],[34,-18],[16,-7]),[[-28,-21],[26,-21]],...[-12,0,12].map(x=>cubic([x,24],[x-7,30],[x+7,34],[x,41]))]);
icon('Food & kitchen','Teapot',[close(cubic([-20,12],[-40,-32],[40,-32],[20,12])),[[-22,15],[22,15]],arc(0,15,5,0,Math.PI),poly([[22,3],[37,20],[40,17],[25,-10]]),cubic([-23,6],[-50,19],[-50,-23],[-23,-12])]);
icon('Food & kitchen','Wine glass',[arc(0,15,20,Math.PI,TAU),[[-20,15],[-15,35],[15,35],[20,15]],[[0,-5],[0,-29]],[[-16,-29],[16,-29]],[[-19,12],[19,12]]]);
icon('Food & kitchen','Artisan bread',[close([...cubic([-33,-12],[-42,33],[42,33],[33,-12]),[-33,-12]]),...[-16,0,16].map(x=>cubic([x-4,0],[x-4,7],[x+2,13],[x+7,15]))]);
icon('Food & kitchen','Chef hat',[close([...cubic([-23,5],[-46,16],[-25,40],[-13,28]),...cubic([-13,28],[-13,49],[18,48],[16,29]).slice(1),...cubic([16,29],[37,41],[45,11],[23,5]).slice(1),[20,-17],[-20,-17],[-23,5]]),[[-21,-9],[21,-9]]]);
icon('Food & kitchen','Honeycomb',[...Array.from({length:7},(_,i)=>{const a=(i-1)*TAU/6,x=i?18*Math.cos(a):0,y=i?18*Math.sin(a):0;return poly(Array.from({length:6},(_,j)=>[x+9*Math.cos(j*TAU/6),y+9*Math.sin(j*TAU/6)]));})]);
icon('Food & kitchen','Apple harvest',[close([...cubic([0,21],[-39,46],[-36,-37],[0,-27]),...cubic([0,-27],[36,-37],[39,46],[0,21]).slice(1)]),cubic([0,21],[-3,30],[4,35],[7,39]),...transform([botanicalLeaf(18,5,1)[0]],2,29,.3)]);
icon('Food & kitchen','Cutlery place setting',[[[-22,30],[-22,9],[-10,9],[-10,30]],[[-18,30],[-18,10]],[[-14,30],[-14,10]],[[-16,9],[-16,-31]],poly([[13,-31],[18,-31],[18,30],[9,15],[9,1],[13,1]])]);
icon('Travel & outdoors','Camping tent',[poly([[-37,-23],[0,33],[37,-23]]),poly([[-20,-23],[0,13],[20,-23]]),[[-44,-23],[44,-23]],[[0,13],[0,-23]]]);
icon('Travel & outdoors','Mountain hiking boot',[poly([[-27,24],[-3,24],[2,1],[29,-9],[32,-23],[-30,-23],[-30,-6],[-27,24]]),[[-29,-17],[31,-17]],...Array.from({length:4},(_,i)=>[[-17,18-i*6],[-3+i,18-i*6]])]);
icon('Travel & outdoors','Trail backpack',[rect(-22,-27,44,52),arc(0,25,10,0,Math.PI),rect(-15,-19,30,18),[[-22,7],[22,7]],[[-15,24],[-15,9]],[[15,24],[15,9]],rect(-28,-18,6,20),rect(22,-18,6,20)]);
icon('Travel & outdoors','Navigation pin',[close([...arc(0,10,20,0,Math.PI),[-20,10],[0,-32],[20,10]]),circle(0,10,7)]);
icon('Travel & outdoors','Caravan',[poly([[-35,-14],[-35,20],[-23,29],[28,29],[35,20],[35,-14]]),rect(-24,6,22,14),rect(10,-14,16,34),circle(-17,-18,8),circle(-17,-18,3),[[35,-14],[46,-14]],circle(21,2,1,12)]);
icon('Travel & outdoors','Trail binoculars',[poly([[-29,-21],[-24,22],[-7,22],[-4,-21]]),poly([[4,-21],[7,22],[24,22],[29,-21]]),[[-6,10],[6,10]],[[-5,0],[5,0]],rect(-30,-26,27,5),rect(3,-26,27,5)]);
icon('Travel & outdoors','Campfire',[...transform([rect(-28,-3,56,6)],0,-24,.25),...transform([rect(-28,-3,56,6)],0,-24,-.25),poly([[0,-18],[-18,-4],[-10,18],[-3,8],[5,35],[19,0],[12,-14]])]);
icon('Travel & outdoors','Travel suitcase',[rect(-32,-22,64,44),[[-10,22],[-10,31],[10,31],[10,22]],[[-19,-22],[-19,22]],[[19,-22],[19,22]],rect(-22,-2,6,9),rect(16,-2,6,9)]);
icon('Garden & growing','Watering can',[poly([[-21,-18],[18,-18],[21,16],[-21,16]]),poly([[18,-6],[38,16],[43,13],[20,-17]]),cubic([-21,12],[-49,20],[-46,-21],[-21,-10]),[[-9,16],[-9,23],[8,23],[8,16]]]);
icon('Garden & growing','Potted seedling',[poly([[-16,0],[16,0],[11,-27],[-11,-27]]),rect(-19,0,38,6),[[0,6],[0,33]],...transform([botanicalLeaf(20,6,1)[0]],0,17,.55),...transform([botanicalLeaf(18,5,2)[0]],0,26,2.65)]);
icon('Garden & growing','Garden trowel',[poly([[-6,7],[-14,-9],[0,-34],[14,-9],[6,7]]),rect(-3,7,6,12),rect(-7,19,14,17),[[0,-25],[0,2]]]);
icon('Garden & growing','Mushroom pair',[close([...arc(0,0,28,0,Math.PI),[-28,0]]),poly([[-6,0],[6,0],[10,-27],[-9,-27]]),circle(-11,12,3),circle(9,18,4),circle(17,6,2)]);
icon('Garden & growing','Dragonfly',[...Array.from({length:4},(_,i)=>transform([botanicalLeaf(32,i<2?6:4,i)[0]],0,i<2?5:0,[.4,Math.PI-.4,-.3,Math.PI+.3][i])).flat(),poly([[-2,10],[2,10],[0,-34]]),circle(0,13,4)]);
icon('Garden & growing','Honey bee',[close(Array.from({length:64},(_,i)=>[15*Math.cos(i*TAU/64),9*Math.sin(i*TAU/64)])),circle(19,0,5),...[-7,0,7].map(x=>[[x,-7],[x,7]]),...transform([botanicalLeaf(20,6,1)[0]],-3,8,1.1),...transform([botanicalLeaf(20,6,1)[0]],6,8,.7),[[21,4],[27,11]],[[23,2],[31,5]]]);
icon('Garden & growing','Butterfly garden gate',[arc(0,0,32,0,Math.PI),[[-32,0],[-32,-28],[32,-28],[32,0]],...[-22,-11,0,11,22].map(x=>[[x,-28],[x,Math.sqrt(32**2-x*x)]]),[[-32,-10],[32,-10]]]);
icon('Garden & growing','Sprouting acorn',[arc(0,0,16,Math.PI,TAU),arc(0,0,18,0,Math.PI),[[-18,0],[18,0]],[[0,18],[3,25]],...[-10,0,10].map(x=>[[x-4,5],[x+2,11]])]);
icon('Celebrations','Wedding ring pair',[circle(-13,0,21),circle(13,0,21),poly([[-21,20],[-13,31],[-5,20],[-13,15]])]);
icon('Celebrations','Birthday cake',[rect(-27,-23,54,31),cubic([-27,0],[-15,-11],[-10,10],[0,0]),cubic([0,0],[12,-11],[17,10],[27,0]),...[-15,0,15].flatMap(x=>[rect(x-2,8,4,14),transform([botanicalLeaf(8,2,1)[0]],x,25,Math.PI/2)[0]]),[[-33,-27],[33,-27]]]);
icon('Celebrations','Wrapped gift',[rect(-28,-27,56,45),rect(-31,18,62,8),[[-4,-27],[-4,26]],[[4,-27],[4,26]],...transform([botanicalLeaf(26,9,1)[0]],0,26,.45),...transform([botanicalLeaf(26,9,1)[0]],0,26,Math.PI-.45)]);
icon('Celebrations','Party balloon trio',[...[-18,0,18].flatMap((x,i)=>[circle(x,18+(i%2)*16,12).map(([px,py])=>[px,py*1.15]),cubic([x,6+(i%2)*16],[x-9,-9],[x+8,-24],[0,-38])])]);
icon('Celebrations','Royal crown',[poly([[-30,-18],[30,-18],[34,20],[15,5],[0,28],[-15,5],[-34,20]]),[[-31,-10],[31,-10]],circle(0,5,4)]);
icon('Celebrations','Love letter',[rect(-34,-23,68,46),[[-34,23],[0,-3],[34,23]],[[-34,-23],[-10,-3]],[[34,-23],[10,-3]],...transform(heart().map(p=>p.map(([x,y])=>[x-50,y-50])),0,6,0,.18)]);
icon('Celebrations','Graduation cap',[poly([[-38,10],[0,28],[38,10],[0,-8]]),cubic([-20,0],[-24,-22],[24,-22],[20,0]),[[30,14],[30,-20],[34,-24]]]);
icon('Celebrations','Anniversary ribbon',[poly([[-30,-10],[30,-10],[30,10],[-30,10]]),poly([[-30,5],[-44,5],[-38,-5],[-44,-15],[-25,-15],[-25,-10]]),poly([[30,5],[44,5],[38,-5],[44,-15],[25,-15],[25,-10]])]);
icon('Animals & pets','Paw print',[close([...cubic([-17,-14],[-23,1],[-8,14],[0,8]),...cubic([0,8],[8,14],[23,1],[17,-14]).slice(1),...cubic([17,-14],[7,-22],[-7,-22],[-17,-14]).slice(1)]),...[-23,-9,9,23].map((x,i)=>circle(x,16+(i===1||i===2?8:0),6))]);
icon('Animals & pets','Cat portrait',[poly([[-24,-12],[-28,29],[-11,18],[11,18],[28,29],[24,-12],[13,-25],[-13,-25]]),[[-13,3],[-6,3]],[[6,3],[13,3]],poly([[-3,-4],[3,-4],[0,-8]]),...[-1,1].flatMap(sign=>Array.from({length:3},(_,i)=>[[sign*12,-8-i*3],[sign*36,-4-i*7]]))]);
icon('Animals & pets','Dog bone',[close([...arc(-24,7,8,0,Math.PI*1.5),...arc(-24,-7,8,Math.PI/2,TAU),[16,-7],...arc(24,-7,8,Math.PI,TAU*1.25),...arc(24,7,8,-Math.PI/2,Math.PI),[-16,7]])]);
icon('Animals & pets','Songbird',[close([...cubic([-27,0],[-13,5],[-10,23],[8,21]),...cubic([8,21],[18,22],[20,14],[21,10]).slice(1),[30,6],[21,3],...cubic([21,3],[8,-22],[-13,-22],[-27,0]).slice(1)]),cubic([-14,0],[-3,9],[5,9],[10,0]),circle(13,13,1.4,16),[[-5,-15],[-8,-23]],[[3,-15],[6,-23]]]);
icon('Animals & pets','Koi fish',[close([...cubic([-20,0],[0,30],[27,17],[32,0]),...cubic([32,0],[27,-17],[0,-30],[-20,0]).slice(1)]),poly([[-20,0],[-36,15],[-31,0],[-36,-15]]),circle(24,4,2,16),cubic([15,13],[8,7],[8,-7],[15,-13]),cubic([-8,0],[1,9],[8,9],[10,4])]);
icon('Animals & pets','Wise owl',[poly([[-23,24],[-13,16],[13,16],[23,24],[25,-11],[14,-27],[-14,-27],[-25,-11]]),circle(-10,3,9),circle(10,3,9),circle(-10,3,3),circle(10,3,3),poly([[-3,-5],[3,-5],[0,-11]]),[[-16,-13],[-10,-19],[-4,-13]],[[4,-13],[10,-19],[16,-13]]]);
icon('Animals & pets','Sea turtle',[circle(0,0,23).map(([x,y])=>[x,y*1.2]),circle(0,35,6),...[-1,1].flatMap(sign=>[poly([[sign*19,14],[sign*36,22],[sign*29,6]]),poly([[sign*19,-14],[sign*31,-25],[sign*23,-27]])]),poly([[-10,-12],[10,-12],[15,7],[0,18],[-15,7]]),[[0,-27],[0,-35]]]);
icon('Animals & pets','Rabbit silhouette',[close([...cubic([-17,8],[-34,58],[-9,61],[-7,15]),...cubic([-7,15],[-3,58],[19,59],[9,12]).slice(1),...cubic([9,12],[41,-16],[8,-40],[-17,-16]).slice(1),...cubic([-17,-16],[-25,-8],[-27,1],[-17,8]).slice(1)]),circle(10,-1,2,16)]);
// Practical blank formats combine a dedicated outer cut outline with optional inner engraving.
const practical=Object.values(icons).flat();
for(let n=0;n<16;n++){const motif=practical[n*3%practical.length];const m=transform(motif.paths,0,0,0,.43);add('purpose-coaster-'+n,motif.name+' coaster','Coasters',[circle(0,0,48,96),circle(0,0,43,96),...m]);add('purpose-bookmark-'+n,motif.name+' bookmark','Bookmarks',[rect(-17,-65,34,130),circle(0,56,2.5,24),...transform(motif.paths,0,26,0,.3),...transform(flourish(n),0,-45,Math.PI/2,.25)]);add('purpose-ornament-'+n,motif.name+' hanging ornament','Hanging ornaments',[circle(0,0,42,80),circle(0,34,2.5,24),...transform(motif.paths,0,-3,0,.65)]);}
for(let n=0;n<12;n++){const shape=n%3===0?circle(0,0,20,64):n%3===1?poly([[0,24],[18,0],[0,-24],[-18,0]]):poly([...arc(0,-3,18,Math.PI,TAU),[0,27]]);const inner=transform([botanicalLeaf(19+Math.floor(n/3)*2,4+n%3,n)[0]],0,-12,Math.PI/2);const pendant=[shape,circle(0,n%3===1?16:n%3===2?18:14,1.5,20),...inner];add('purpose-earrings-'+n,['Round','Diamond','Teardrop'][n%3]+' botanical earring pair '+(1+Math.floor(n/3)),'Jewellery', [...transform(pendant,-25,0),...transform(pendant,25,0)]);}
});

// Original architectural illustrations; landmarks are stylised engraving studies.
const expLine=(...points)=>points;
function expAdd(key,name,category,p){add('expansion-'+key,name,category,p);}
const archWindow=(x,y,w,h)=>[...arc(x+w/2,y+h-w/2,w/2,0,Math.PI,18),[x,y],[x+w,y],[x+w,y+h-w/2]];
function expWindows(x,y,cols,rows,w=5,h=8,gap=7){const p=[];for(let r=0;r<rows;r++)for(let c=0;c<cols;c++)p.push(rect(x+c*gap,y+r*(h+5),w,h));return p;}
register('landmarks', () => {
let opera=[[[0,0],[135,0]],[[5,4],[130,4]]];
for(const [x,peakX,peakY,end] of [[12,22,35,61],[30,43,61,82],[61,82,48,104],[88,111,28,127]]){const shell=close([...cubic([x,7],[x+2,peakY*.38],[peakX-3,peakY*.85],[peakX,peakY]),...cubic([peakX,peakY],[peakX+2,peakY*.65],[end-12,9],[end,7]).slice(1)]);opera=overlay(opera,[shell]);for(let j=1;j<5;j++)opera.push(cubic([x+(end-x)*j/5,7],[peakX+(end-peakX)*j/8,peakY*.25],[peakX+j*.3,peakY*.72],[peakX,peakY]));}
expAdd('opera-house','Sydney Opera House · sail roofs','World landmarks',opera);
let bridge=[[[0,2],[150,2]],[[0,10],[150,10]],rect(10,10,12,45),rect(128,10,12,45),rect(8,55,16,5),rect(126,55,16,5)];for(const r of [1,.86])bridge.push(Array.from({length:81},(_,i)=>{const x=22+i*106/80;return[x,12+r*52*Math.sin(i/80*Math.PI)];}));for(let i=1;i<16;i++){const x=22+i*106/16,y=12+52*Math.sin(i/16*Math.PI);bridge.push([[x,10],[x,y]]);if(i<15)bridge.push([[x,y],[x+106/16,12+.86*52*Math.sin((i+1)/16*Math.PI)]]);}expAdd('harbour-bridge','Sydney Harbour Bridge · steel arch','World landmarks',bridge);
let eiffel=[poly([[-31,0],[-21,0],[-8,32],[-3,69],[0,98],[3,69],[8,32],[21,0],[31,0],[15,32],[7,68],[2,94],[0,108],[-2,94],[-7,68],[-15,32]]),rect(-18,30,36,4),rect(-9,64,18,3),arc(0,0,13,0,Math.PI),[[-35,-3],[35,-3]]];for(const side of [-1,1])for(let j=0;j<6;j++){const y=5+j*9,x=side*(24-y*.3);eiffel.push([[x,y],[side*(19-(y+8)*.24),y+8]],[[side*(19-y*.24),y],[side*(24-(y+8)*.3),y+8]]);}expAdd('eiffel-tower','Eiffel Tower · lattice elevation','World landmarks',eiffel);
let liberty=[rect(-20,0,40,10),rect(-14,10,28,13),poly([[-12,23],[10,23],[7,48],[16,55],[12,62],[7,58],[3,74],[-6,74],[-12,53],[-22,87],[-26,85],[-20,45],[-11,47]]),circle(-2,81,6,32),poly([[-9,85],[-15,92],[-6,89],[-5,99],[-1,91],[4,98],[4,89],[13,92],[5,84]]),rect(-28,87,7,6),poly([[-28,93],[-29,100],[-25,109],[-22,101],[-21,93]]),rect(8,48,9,16)];for(const x of [-7,-2,3])liberty.push([[x,27],[x+2,64]]);expAdd('statue-liberty','Statue of Liberty · torch and crown','World landmarks',liberty);
let taj=[rect(-48,0,96,7),rect(-28,7,56,32),close([...arc(0,39,21,0,Math.PI,40),[-21,39]]),[[0,60],[0,68]],archWindow(-7,7,14,24),...expWindows(-23,15,2,1,5,10,9),...expWindows(9,15,2,1,5,10,9)];for(const x of [-43,36])taj.push(rect(x,7,7,51),arc(x+3.5,58,5,0,Math.PI,18));expAdd('taj-mahal','Taj Mahal · dome and minarets','World landmarks',taj);
let bigben=[rect(-13,0,26,70),rect(-16,70,32,27),circle(0,84,10),[[0,84],[0,91]],[[0,84],[6,81]],poly([[-15,97],[15,97],[8,111],[0,130],[-8,111]]),...expWindows(-8,8,2,4,5,7,11),[[-20,0],[20,0]]];expAdd('elizabeth-tower','Elizabeth Tower · London clock tower','World landmarks',bigben);
let pisa=[rect(-24,0,48,5)];for(let j=0;j<7;j++){const x=-19+j*1.3,y=5+j*12;pisa.push(rect(x,y,38,3));for(let k=0;k<6;k++)pisa.push(archWindow(x+2+k*6,y+3,4,9));}pisa.push(rect(-8,89,25,4),...expWindows(-5,93,4,1,3,6,6));expAdd('pisa','Leaning Tower of Pisa · arcades','World landmarks',pisa);
let colosseum=[rect(-51,0,102,5)];for(let j=0;j<3;j++){const w=100-j*6;colosseum.push(rect(-w/2,5+j*15,w,3));for(let k=0;k<9-j;k++)colosseum.push(archWindow(-w/2+3+k*(w-6)/(9-j),8+j*15,7,11));}colosseum.push([[-44,50],[24,50],[24,39],[38,39],[38,29],[47,29]]);expAdd('colosseum','Roman Colosseum · tiered arches','World landmarks',colosseum);
let gate=[rect(-33,0,66,5),rect(-31,5,11,40),rect(20,5,11,40),rect(-35,45,70,7),rect(-28,52,56,5),archWindow(-20,5,40,36),...expWindows(-29,14,1,3,6,5,8),...expWindows(23,14,1,3,6,5,8)];expAdd('arc-triomphe','Arc de Triomphe · monumental arch','World landmarks',gate);
let pyramids=[poly([[-60,0],[-20,58],[18,0]]),[[-20,58],[-8,0]],poly([[5,0],[39,40],[75,0]]),[[39,40],[48,0]],poly([[-80,0],[-57,27],[-40,0]]),[[-90,-4],[85,-4]],circle(54,56,7)];expAdd('giza','Pyramids of Giza · desert skyline','World landmarks',pyramids);
let pagoda=[];for(let j=0;j<5;j++){const y=j*16,w=42-j*6;pagoda.push(poly([[-w,y],[w,y],[w+5,y+6],[w-7,y+3],[0,y+15],[-w+7,y+3],[-w-5,y+6]]));if(j<4)pagoda.push(rect(-w+9,y+6,2*w-18,10));}pagoda.push([[0,79],[0,92]],...expWindows(-18,0,4,1,4,7,10));expAdd('five-storey-pagoda','Japanese five-storey pagoda','World landmarks',pagoda);
let golden=[[[0,10],[130,10]],[[0,6],[130,6]],rect(24,0,5,61),rect(99,0,5,61),cubic([0,25],[14,28],[21,50],[26,60]),cubic([26,60],[48,-3],[80,-3],[102,60]),cubic([102,60],[113,39],[124,29],[130,25])];for(let i=1;i<15;i++){const t=i/15,x=26+t*76,y=60-189*t*(1-t);golden.push([[x,10],[x,y]]);}expAdd('golden-gate','Golden Gate Bridge · suspension span','World landmarks',golden);
});
// Heritage buildings: distinct structure profiles and architectural details.
const heritageNames=['English thatched cottage','French village bakery','Victorian terrace house','Old stone chapel','Dutch canal house','Alpine timber chalet','Tuscan farmhouse','Georgian town house','Art Deco cinema','Timber railway station','Colonial veranda house','Medieval gatehouse','Country watermill','Historic fire station','Old coastal warehouse','Half-timbered inn','Prairie schoolhouse','Stone blacksmith forge','Traditional windmill','Vintage general store'];
register('architecture', () => {
for(let n=0;n<20;n++){let p=[],w=56+n%4*8,h=28+n%3*12;const roof=n%5;if(roof===0)p.push(poly([[-w/2-5,h],[0,h+26],[w/2+5,h],[w/2,h-2],[-w/2,h-2]]));else if(roof===1)p.push(poly([[-w/2-5,h],[-w/2+8,h+18],[w/2-8,h+18],[w/2+5,h]]));else if(roof===2)p.push(poly([[-w/2,h],[-w/2,h+5],[-12,h+5],[-12,h+13],[-6,h+13],[0,h+24],[6,h+13],[12,h+13],[12,h+5],[w/2,h+5],[w/2,h]]));else p.push(rect(-w/2-3,h,w+6,5));p.push(rect(-w/2,0,w,h),rect(-5,0,10,18),...expWindows(-w/2+5,8,2,1,7,9,12),...expWindows(10,8,2,1,7,9,12));if(h>30)p.push(...expWindows(-w/2+6,28,4,1,6,8,(w-12)/4));if(n%4===0)p.push(rect(w/2-14,h+8,7,18));if(n%4===1)for(let x=-w/2+3;x<w/2;x+=9)p.push([[x,h],[x+7,h+15]]);if(n===3||n===11){p.push(rect(w/2,0,18,h+22),poly([[w/2-2,h+22],[w/2+9,h+45],[w/2+20,h+22]]),archWindow(w/2+4,h+3,9,15));}if(n===10){p.push([[-w/2-8,23],[w/2+8,23]],[[-w/2-7,0],[-w/2-7,23]],[[w/2+7,0],[w/2+7,23]]);}if(n===12){p.push(circle(w/2+12,12,16),...Array.from({length:8},(_,i)=>transform([[[0,0],[16,0]]],w/2+12,12,i*TAU/8)).flat());}if(n===15)for(let x=-w/2+3;x<w/2-3;x+=15)p.push([[x,0],[x+12,h]],[[x,h],[x+12,0]]);if(n===18){p=[poly([[-22,0],[22,0],[11,52],[-11,52]]),poly([[-16,52],[0,68],[16,52]])];for(let j=0;j<4;j++)p.push(...transform([rect(3,-3,40,7),...Array.from({length:6},(_,i)=>[[8+i*6,-3],[8+i*6,4]])],0,42,j*Math.PI/2+.3));}p.push([[-w/2-12,-3],[w/2+12,-3]]);expAdd('heritage-'+n,heritageNames[n],'Historic buildings',p);}
});
function expPine(x,y,h=20,w=8){return [poly([[x,y+h],[x-w*.45,y+h*.65],[x-w*.2,y+h*.65],[x-w*.8,y+h*.33],[x-w*.4,y+h*.33],[x-w,y],[x+w,y],[x+w*.4,y+h*.33],[x+w*.8,y+h*.33],[x+w*.2,y+h*.65],[x+w*.45,y+h*.65]]),[[x,y],[x,y-4]]];}
function expScene(n,river){let p=[];const peaks=[[-55,0],[-34,25+n%4*3],[-12,5],[10,37+n%3*5],[33,3],[53,22+n%5*2],[70,-4]];p.push(peaks,[[-42,15],[-34,10],[-26,17]],[[-1,24],[10,17],[22,24]]);const sun=circle(-40,40,5+n%4,32);p.push(sun);if(river){p.push(cubic([0,4],[-48,-10],[45,-15],[-23,-54]),cubic([6,4],[-30,-10],[65,-15],[6,-54]));for(let j=0;j<4;j++)p.push(cubic([-50,-10-j*10],[-43,-7-j*10],[-38,-11-j*10],[-32,-9-j*10]));}else for(let j=0;j<4;j++)p.push(cubic([-53+j*4,-9-j*9],[-25,-5-j*9],[22,-17-j*7],[62-j*3,-7-j*9]));for(let j=0;j<3+n%4;j++)p.push(...expPine(-56+j*8,-21-j%3*5,11+(j+n)%4*4,4));if(n%4===1)p.push(...transform(sail,35,-19,0,.35));if(n%4===2)p.push(...transform([poly([[-10,0],[0,15],[10,0]]),poly([[-5,0],[0,8],[5,0]])],37,-25));if(n%4===3)p.push(cubic([18,38],[21,42],[24,42],[27,38]),cubic([27,38],[30,42],[33,42],[36,38]));return p;}
register('scenery', () => {
for(let n=0;n<16;n++){expAdd('mountain-'+n,['Alpine lake','Pine ridge','Summit campsite','Highland panorama'][n%4]+' · scene '+(1+Math.floor(n/4)),'Mountain scenes',expScene(n,false));expAdd('river-'+n,['Meandering river','Riverside sailing','Woodland river camp','River valley birds'][n%4]+' · scene '+(1+Math.floor(n/4)),'River scenes',expScene(n,true));}
});
const portraitNames=['Cat','Dog','Lion','Tiger','Fox','Bear','Panda','Owl','Rabbit','Mouse','Bat','Frog','Seal','Monkey','Koala','Pig','Cow','Deer','Wolf','Horse','Sheep','Goat','Hedgehog','Raccoon'];
register('portraits', () => {
for(let n=0;n<24;n++){let p=[],face=circle(0,0,24,70).map(([x,y])=>[x,y*(n===19?1.4:1)]);if(n===2)p.push(close(Array.from({length:96},(_,i)=>{const a=i*TAU/96,r=33+3*Math.sin(a*16);return[r*Math.cos(a),r*Math.sin(a)];})));if(n===22)p.push(close(Array.from({length:50},(_,i)=>{const a=i*TAU/50,r=i%2?26:36;return[r*Math.cos(a),r*Math.sin(a)];})));if([0,3,4,7,10,18].includes(n))for(const side of [-1,1])p.push(poly([[side*9,18],[side*26,35],[side*24,0]]));else if(n===8)for(const side of [-1,1])p.push(...transform([botanicalLeaf(40,7,0)[0]],side*10,17,Math.PI/2-side*.15));else if(n===1||n===19)for(const side of [-1,1])p.push(...transform([botanicalLeaf(n===1?34:25,9,1)[0]],side*17,17,n===1?-Math.PI/2:Math.PI/2));else if(n!==12&&n!==22&&n!==11)for(const side of [-1,1])p.push(circle(side*22,20,n===9||n===14?12:8,32));p=overlay(p,[face]);if(n===7){p.push(circle(-10,4,10,32),circle(10,4,10,32));}if(n===6||n===23)for(const side of [-1,1])p.push(close(Array.from({length:36},(_,i)=>[side*10+8*Math.cos(i*TAU/36),4+6*Math.sin(i*TAU/36)])));p.push(circle(-9,5,2.2,20),circle(9,5,2.2,20));if(n===15||n===16||n===19){p.push(circle(0,-10,10,40).map(([x,y])=>[x*1.3,y]),circle(-5,-10,1.5,16),circle(5,-10,1.5,16));}else{p.push(poly([[-4,-3],[4,-3],[0,-7]]),[[0,-7],[0,-12]],cubic([-7,-11],[-5,-17],[-2,-16],[0,-12]),cubic([0,-12],[2,-16],[5,-17],[7,-11]));}if([0,3,4,9,12].includes(n))for(const side of [-1,1])for(let j=0;j<3;j++)p.push([[side*10,-7-j*3],[side*33,-3-j*7]]);if(n===3)for(const side of [-1,1])for(let j=0;j<3;j++)p.push(poly([[side*23,9-j*8],[side*14,7-j*8],[side*23,5-j*8]]));if(n===17)for(const side of [-1,1])p.push([[side*14,20],[side*18,44],[side*10,52]],[[side*18,39],[side*29,46],[side*29,53]],[[side*18,44],[side*21,54]]);if(n===16||n===21)for(const side of [-1,1])p.push(cubic([side*15,19],[side*36,29],[side*35,36],[side*28,44]));if(n===20)for(let i=0;i<8;i++)p.push(arc(-21+i*6,20,5,0,Math.PI,16));if(n===11)p.push(arc(-11,16,7,0,Math.PI),arc(11,16,7,0,Math.PI));if(n===10)for(const side of [-1,1])p.push(poly([[side*23,12],[side*55,22],[side*47,2],[side*38,5],[side*31,-7],[side*21,-5]]));if(n===4)p.push([[-20,7],[-11,-4],[0,-8],[11,-4],[20,7]]);if(n===13)p.push(arc(0,-7,14,Math.PI,TAU),arc(0,4,17,0,Math.PI));if(n===23)p.push([[-21,11],[-10,15],[0,10],[10,15],[21,11]],[[0,10],[0,18]]);expAdd('portrait-'+n,portraitNames[n]+' woodland portrait','Animal portraits',p);}
});
const insectKinds=['Ladybird','Stag beetle','Dragonfly','Honey bee','Butterfly','Grasshopper'];
register('bugs', () => {
for(let kind=0;kind<6;kind++)for(let style=0;style<3;style++){let p=[],len=20+style*4;if(kind===0||kind===1){p.push(circle(0,0,len,64).map(([x,y])=>[x*.7,y]),circle(0,len+6,7,32),[[0,-len],[0,len]]);for(const side of [-1,1])for(let j=0;j<3;j++)p.push([[side*len*.55,10-j*10],[side*(len+8),18-j*14],[side*(len+12),12-j*14]]);if(kind===0)for(const side of [-1,1])for(let j=0;j<2+style;j++)p.push(circle(side*(6+(j%2)*2),10-j*8,2.3,16));else for(const side of [-1,1])p.push([[side*3,len+11],[side*10,len+23],[side*5,len+27]],[[side*7,len+19],[side*2,len+21]]);}else if(kind===2||kind===4){for(const side of [-1,1])for(let j=0;j<2;j++){const a=side===1?(j?.0:.6):Math.PI-(j?.0:.6);p.push(...transform([botanicalLeaf(35+style*3,kind===4?13-j*3:5,j+style)[0]],0,j?-5:7,a));if(kind===4)p.push(...transform([botanicalLeaf(23,6,style)[0]],side*5,j?-4:9,a));}p.push(poly([[-2,14],[2,14],[0,-35-style*3]]),circle(0,17,4));}else if(kind===3){p.push(circle(0,0,20,64).map(([x,y])=>[x*.65,y]),circle(0,26,6));for(let j=0;j<4;j++)p.push([[-10,-12+j*8],[10,-12+j*8]]);for(const side of [-1,1])p.push(...transform([botanicalLeaf(32+style*5,9,style)[0]],side*7,12,side===1?.7:Math.PI-.7));}else{p.push(poly([[-27,0],[16,0],[25,12],[16,18],[-27,8]]),circle(19,11,2),[[-15,4],[-4,24],[5,-8]],[[4,2],[10,-10],[24,-10]],[[-24,0],[-30,-9],[-38,-9]],[[21,17],[31,28]],[[16,18],[19,32]],cubic([-24,6],[-9,18],[9,6],[14,3]));if(style>0)p.push([[-25,8],[-12,17],[8,13]]);if(style===2)p.push([[-23,4],[-15,-13],[-28,-18]]);}if(kind!==5)for(const side of [-1,1])p.push(cubic([side*3,len+7],[side*8,len+14],[side*12,len+13],[side*11,len+18]));expAdd('insect-'+kind+'-'+style,insectKinds[kind]+' · wing and body study '+(style+1),'Insects',p);}
});
// Scalable craft and play artwork: no thickness, tolerances or mechanical assembly implied.
register('play', () => {
const toyArt=[];const toy=(name,p)=>toyArt.push({name,paths:p});
toy('Rocking horse', [poly([[-26,5],[-12,20],[6,20],[14,38],[25,34],[31,20],[19,24],[15,10],[20,-9],[12,-9],[5,5],[-10,4],[-18,-10],[-25,-10],[-21,5]]),arc(0,7,37,Math.PI*1.15,Math.PI*1.85,48),circle(22,29,1.4,16)]);
toy('Pull-along duck',[close([...cubic([-29,0],[-16,-18],[24,-20],[25,5]),...cubic([25,5],[33,22],[13,29],[9,11]).slice(1),[0,8],[-22,12],[-29,0]]),poly([[26,13],[37,9],[26,6]]),circle(21,17,1.3),circle(-15,-16,6),circle(16,-16,6)]);
toy('Toy locomotive',[rect(-32,-5,64,20),rect(-30,15,22,22),rect(16,15,8,15),rect(-33,37,29,4),rect(-25,23,12,9),...[-23,0,23].map(x=>circle(x,-10,8)),[[-23,-10],[23,-10]]]);
toy('Wooden toy aeroplane',[poly([[-40,0],[-6,6],[4,30],[13,30],[10,6],[38,3],[38,-3],[10,-6],[13,-30],[4,-30],[-6,-6],[-40,0]]),[[-26,2],[-28,15],[-21,15],[-13,4]]]);
toy('Sailing bath boat',[...sail,...[-12,0,12].map(x=>circle(x,-17,2,16)),[[-28,-29],[28,-29]]]);
toy('Toy robot',[rect(-18,7,36,27),rect(-23,-23,46,28),circle(-8,23,3),circle(8,23,3),[[-8,13],[8,13]],[[0,34],[0,44]],circle(0,46,3),rect(-33,-15,8,19),rect(25,-15,8,19),rect(-17,-36,10,13),rect(7,-36,10,13),circle(0,-8,6)]);
toy('Teddy bear',[circle(-16,25,9),circle(16,25,9),circle(0,13,21),circle(0,-20,23),circle(-23,-10,10),circle(23,-10,10),circle(-14,-40,11),circle(14,-40,11),circle(-7,17,2),circle(7,17,2),circle(0,7,7),circle(0,9,2)]);
toy('Spinning top',[poly([[-28,6],[0,-29],[28,6]]),arc(0,6,28,0,Math.PI,40).map(([x,y])=>[x,6+(y-6)*.3]),rect(-3,14,6,16),[[-21,-2],[21,-2]],[[-13,-12],[13,-12]]]);
toy('Toy castle',[poly([[-34,-24],[34,-24],[34,27],[25,27],[25,19],[16,19],[16,27],[7,27],[7,15],[-7,15],[-7,27],[-16,27],[-16,19],[-25,19],[-25,27],[-34,27]]),archWindow(-8,-24,16,25),rect(-27,-4,6,12),rect(21,-4,6,12)]);
toy('Toy sailing submarine',[close([...arc(25,0,15,-Math.PI/2,Math.PI/2),[-25,15],...arc(-25,0,15,Math.PI/2,Math.PI*1.5),[25,-15]]),rect(-6,15,18,9),[[2,24],[2,34],[14,34]],...[-20,0,20].map(x=>circle(x,0,6))]);
toy('Toy giraffe',[poly([[-26,-22],[-20,-22],[-17,2],[2,2],[7,-22],[13,-22],[10,8],[19,39],[32,39],[32,48],[17,51],[6,15],[-21,13]]),circle(26,45,1.5),...[[3,15],[8,28],[-8,8],[-18,7]].map(([x,y])=>circle(x,y,2.5))]);
toy('Toy dinosaur',[close([...cubic([-35,-4],[-6,30],[8,24],[18,19]),[18,32],[34,31],[39,24],[30,21],[24,22],[20,4],[24,-20],[17,-20],[10,-2],[-8,-2],[-14,-20],[-21,-20],[-17,1],[-35,-4]]),circle(31,27,1.5),poly([[-20,10],[-18,21],[-10,20],[-8,28],[0,23],[7,29],[10,21]])]);
{let bear=[];for(const [x,y,r]of [[-16,25,9],[16,25,9],[-23,-10,10],[23,-10,10],[-14,-40,11],[14,-40,11],[0,-20,23],[0,13,21],[0,7,7]])bear=overlay(bear,[circle(x,y,r)]);bear.push(circle(-7,17,2),circle(7,17,2),circle(0,9,2));toyArt.find(t=>t.name==='Teddy bear').paths=bear;}
for(let n=0;n<toyArt.length;n++){const t=toyArt[n];expAdd('toy-'+n,t.name+' · craft outline','Children’s toy artwork',t.paths);}
const playShapes=[circle(0,0,14,48),rect(-13,-13,26,26),poly([[-16,-13],[16,-13],[0,16]]),poly([[0,18],[14,0],[0,-18],[-14,0]]),star(5).map(p=>p.map(([x,y])=>[(x-50)*.35,(y-50)*.35]))[0],poly(Array.from({length:6},(_,i)=>[16*Math.cos(i*TAU/6),16*Math.sin(i*TAU/6)]))];
for(let n=0;n<6;n++){const shapes=[playShapes[n],playShapes[(n+1)%6],playShapes[(n+3)%6]];const p=[rect(-62,-24,124,48),...shapes.flatMap((q,i)=>transform([q],(i-1)*40,0)),...shapes.flatMap((q,i)=>transform([q],(i-1)*40,-57))];expAdd('matching-board-'+n,['Circle','Square','Triangle','Diamond','Star','Hexagon'][n]+' matching board and tokens','Learning & play',p);}
for(let n=0;n<6;n++){const p=[rect(-40,-40,80,80)],count=3+n;for(let i=0;i<count;i++){const a=i*TAU/count;p.push(...transform([playShapes[n]],25*Math.cos(a),25*Math.sin(a),a,.45));}p.push(circle(0,0,5,24));expAdd('counting-card-'+n,`${count}-shape counting card`,'Learning & play',p);}
});

// Original vehicle and music studies. Open detail paths are intended for engraving.
register('vehicles-and-music', () => {
(function(){
const put=(id,name,cat,p)=>add('expansion-vm-'+id,name,cat,p);
const wheel=(x,y,r=9)=>[circle(x,y,r,40),circle(x,y,r*.43,24)];
const ellipse=(x,y,rx,ry)=>circle(0,0,1,48).map(([a,b])=>[x+a*rx,y+b*ry]);
const line=(...p)=>p;
const radial=(x,y,r,n)=>Array.from({length:n},(_,i)=>[[x,y],[x+r*Math.cos(TAU*i/n),y+r*Math.sin(TAU*i/n)]]);
const car=(roof,details=[],back=-48,front=48)=>[...wheel(-29,0),...wheel(29,0),[[back,0],[back,15],...roof,[front,14],[front,0],[39,0]],[[19,0],[-19,0]],[[-39,0],[back,0]],...details];
const cat='Cars & road vehicles';
put('saloon','Classic four-door saloon',cat,car([[-33,20],[-18,37],[15,37],[31,19]],[poly([[-27,21],[-15,33],[-3,33],[-3,21]]),poly([[1,21],[1,33],[12,33],[25,21]]),[[-3,19],[-3,4]],[[1,19],[1,4]],[[4,16],[10,16]],rect(39,11,7,3)]));
put('sports','Low sports coupe',cat,car([[-37,16],[-16,29],[10,31],[30,17]],[poly([[-26,19],[-13,26],[8,27],[21,19]]),[[15,16],[13,6],[-9,6]],[[35,14],[44,12]],[[8,13],[13,13]]]));
put('vintage','1930s upright touring car',cat,car([[-38,18],[-27,40],[8,40],[13,21],[33,21]],[rect(-23,23,12,13),rect(-7,23,12,13),[[12,21],[12,5]],...Array.from({length:5},(_,i)=>[[18+i*3,17],[18+i*3,9]]),[[-45,7],[-39,14],[-22,14],[-16,6]],[[19,7],[25,14],[34,14],[40,7]]],-49,48));
put('hotrod','Open hot rod with exposed engine',cat,car([[-40,17],[-22,17],[-16,29],[-11,29],[-11,18],[15,18],[15,22],[36,22]],[rect(16,14,19,7),...Array.from({length:4},(_,i)=>[[20+i*4,14],[20+i*4,7],[26+i*4,7]]),[[-14,18],[-15,27]],[[1,18],[1,8]]],-49,46));
put('convertible','Open-top beach convertible',cat,car([[-40,19],[-22,19],[-17,13],[9,13],[14,31],[17,31],[19,18],[34,17]],[cubic([-19,14],[-20,27],[-8,26],[-8,14]),cubic([-1,14],[-1,26],[10,26],[10,14]),[[20,14],[24,7],[8,7]]]));
put('hatchback','Compact city hatchback',cat,car([[-40,16],[-32,34],[8,34],[26,19]],[poly([[-33,20],[-28,30],[-7,30],[-7,20]]),poly([[-3,20],[-3,30],[6,30],[19,20]]),[[-5,18],[-5,5]],rect(39,10,7,4)]));
put('estate','Long-roof estate wagon',cat,car([[-40,33],[12,33],[29,19]],[rect(-36,21,16,8),rect(-16,21,17,8),poly([[5,21],[5,29],[10,29],[23,21]]),[[-18,19],[-18,5]],[[3,19],[3,5]],[[-39,37],[13,37]]]));
put('pickup','Single-cab utility pickup',cat,car([[-46,25],[-9,25],[-9,40],[13,40],[24,24]],[rect(-43,10,31,11),poly([[-5,27],[-5,36],[10,36],[18,27]]),[[-8,24],[-8,4]],[[10,22],[15,22]]]));
put('doublecab','Twin-cab off-road utility',cat,car([[-46,24],[-24,24],[-24,39],[14,39],[26,23]],[rect(-43,10,16,10),rect(-20,26,13,9),poly([[-3,26],[-3,35],[12,35],[20,26]]),[[-5,23],[-5,5]],[[-39,-12],[39,-12]] ]));
put('deliveryvan','High-roof delivery van',cat,car([[-45,45],[14,45],[31,24]],[poly([[13,28],[13,40],[27,28]]),rect(-36,20,38,18),[[8,40],[8,5]],[[11,23],[15,23]]]));
put('minibus','Community passenger minibus',cat,car([[-45,41],[24,41],[36,24]],[...Array.from({length:4},(_,i)=>rect(-39+i*15,25,11,11)),[[23,22],[23,6]],rect(33,7,10,5)]));
put('bus','Long-distance coach',cat,[...wheel(-37,0),...wheel(37,0),[[-57,0],[-57,38],[48,38],[57,25],[57,0],[47,0]],[[27,0],[-27,0]],[[-47,0],[-57,0]],...Array.from({length:6},(_,i)=>rect(-51+i*15,22,11,11)),[[-54,16],[53,16]],rect(-23,5,40,7),[[46,19],[46,3]]]);
put('doubledecker','City double-decker bus',cat,[...wheel(-32,0),...wheel(32,0),[[-49,0],[-49,57],[45,57],[49,45],[49,0],[42,0]],[[22,0],[-22,0]],[[-42,0],[-49,0]],...Array.from({length:5},(_,i)=>rect(-43+i*18,39,13,12)),...Array.from({length:4},(_,i)=>rect(-43+i*18,20,13,12)),rect(30,7,12,26),[[-45,35],[45,35]]]);
put('boxtruck','Box-bodied freight truck',cat,[...wheel(-33,0),...wheel(34,0),rect(-51,13,59,42),poly([[12,13],[12,42],[33,42],[49,24],[49,7],[44,7]]),poly([[17,27],[17,37],[30,37],[39,27]]),[[-49,7],[23,7]],[[17,22],[23,22]],rect(-46,18,49,32)]);
put('tanker','Road tanker with cylindrical tank',cat,[...wheel(-37,0),...wheel(-17,0),...wheel(36,0),poly([[14,12],[14,40],[32,40],[49,23],[49,6],[46,6]]),poly([[19,26],[19,35],[29,35],[38,26]]),[[-50,10],[23,10]],...transform([ellipse(0,0,31,17)],-20,31),rect(-28,48,16,4)]);
put('fireengine','Fire engine with roof ladder',cat,[...wheel(-34,0),...wheel(35,0),[[-50,8],[-50,37],[17,37],[17,45],[35,45],[49,29],[49,8],[45,8]],rect(-44,14,20,17),rect(-19,14,20,17),poly([[22,29],[22,40],[32,40],[42,29]]),rect(-47,41,56,6),...Array.from({length:8},(_,i)=>[[-43+i*7,41],[-43+i*7,47]]),circle(5,21,6),rect(22,47,10,4)]);
put('ambulance','Emergency ambulance outline',cat,car([[-44,43],[12,43],[28,25]],[rect(-33,19,24,17),poly([[-24,21],[-18,21],[-18,25],[-14,25],[-14,31],[-18,31],[-18,35],[-24,35],[-24,31],[-28,31],[-28,25],[-24,25]]),poly([[15,28],[15,38],[24,28]]),rect(-12,45,20,4)]));
put('tractor','Farm tractor with rear mudguard',cat,[...wheel(-25,0,17),...wheel(32,-6,10),[[ -43,10],[-43,30],[-8,30],[-8,10]],poly([[-18,30],[-18,55],[5,55],[5,27],[32,27],[40,17],[40,5],[20,5]]),rect(-13,33,13,17),rect(21,28,4,19),...Array.from({length:4},(_,i)=>[[23+i*4,12],[23+i*4,22]]),[[-10,-10],[19,-10]]]);
put('excavator','Tracked excavator',cat,[poly([[-41,-9],[31,-9],[39,-2],[31,5],[-41,5],[-48,-2]]),...Array.from({length:5},(_,i)=>circle(-35+i*14,-2,4,20)),poly([[-29,8],[-29,34],[-8,34],[2,20],[2,8]]),poly([[-24,20],[-24,30],[-12,30],[-5,20]]),poly([[1,18],[14,47],[25,49],[47,22],[42,18],[22,39],[10,14]]),poly([[41,17],[52,22],[60,6],[46,6]])]);
put('forklift','Warehouse forklift',cat,[...wheel(-21,0,9),...wheel(17,0,7),poly([[-31,6],[-31,26],[-6,26],[-6,10],[24,10],[24,6]]),[[-21,26],[-21,47],[12,47],[12,10]],[[21,8],[21,45],[26,45],[26,0],[45,0]],rect(-19,48,33,3),[[-3,13],[-3,24],[6,24]]]);
put('camper','Vintage camper van',cat,car([[-43,42],[27,42],[39,26]],[rect(-35,25,23,11),rect(-6,25,21,11),poly([[20,26],[20,37],[28,37],[34,26]]),[[-43,20],[43,20]],[[17,23],[17,5]],rect(-23,44,35,5)]));
put('caravan','Touring caravan trailer',cat,[...wheel(0,0,10),poly([[-42,7],[-42,35],[-34,43],[29,43],[39,31],[39,7],[11,7]]),[[-11,7],[-42,7]],rect(-33,23,23,13),rect(15,10,14,27),circle(25,23,1.2),[[39,9],[53,3],[57,3]]]);
put('formula','Open-wheel circuit racing car',cat,[...wheel(-31,0,12),...wheel(31,0,12),poly([[-26,9],[-12,11],[-4,22],[9,22],[14,12],[43,5],[43,0],[16,0],[10,-4],[-16,-4]]),rect(-46,14,18,4),rect(35,-5,16,4),arc(4,18,5,0,Math.PI),[[2,22],[0,29],[-6,29]]]);
put('rally','Rally coupe with auxiliary lamps',cat,car([[-38,18],[-19,35],[12,35],[28,19]],[poly([[-28,22],[-16,31],[-3,31],[-3,22]]),poly([[1,22],[1,31],[10,31],[21,22]]),circle(40,14,3),circle(32,14,3),rect(-37,21,10,3),[[-17,19],[-12,6],[10,6],[14,18]],circle(-1,13,5)]));
put('taxi','City taxi with roof sign',cat,car([[-36,20],[-18,36],[15,36],[31,20]],[rect(-8,38,17,7),poly([[-27,23],[-15,32],[-3,32],[-3,23]]),poly([[2,23],[2,32],[12,32],[24,23]]),...Array.from({length:6},(_,i)=>rect(-18+i*7,13,4,4))]));
const moto=(body,extra=[])=>[...wheel(-30,0,13),...wheel(31,0,13),[[-30,0],[-10,21],[7,0],[-30,0]],[[7,0],[19,22],[31,0]],[[19,22],[16,31],[7,31]],...body,...extra];
const mc='Motorbikes & cycles';
put('roadster','Classic twin-cylinder road motorcycle',mc,moto([poly([[-15,24],[-12,32],[3,34],[14,27],[9,22]]),[[-22,25],[-10,25]],ellipse(-2,9,8,7),[[5,7],[13,4],[24,4]]],[arc(-30,0,16,.2,2.7),arc(31,0,16,.2,2.7),circle(22,29,4)]));
put('chopper','Long-fork custom chopper',mc,[...wheel(-34,0,15),...wheel(39,0,12),[[-34,0],[-12,18],[6,0],[-34,0]],[[6,0],[17,31],[39,0]],[[17,31],[13,39],[3,39]],poly([[-11,21],[-9,29],[7,28],[11,22]]),[[-24,22],[-13,22]],ellipse(-1,10,7,8),[[-20,22],[-27,31]],[[4,5],[25,5]],arc(-34,0,18,.15,2.8)]);
put('sportbike','Fully faired sport motorcycle',mc,moto([poly([[-24,25],[-10,25],[0,34],[15,32],[25,22],[11,-1],[-4,1],[-9,18]]),poly([[13,34],[18,41],[27,34],[25,28]]),[[1,24],[14,21],[5,8]],[[0,5],[18,3]]],[arc(31,0,16,.4,2.6)]));
put('dirtbike','Off-road motocross motorcycle',mc,moto([poly([[-28,31],[-8,30],[4,34],[12,29],[8,23],[-13,25]]),[[-31,27],[-40,23]],[[22,25],[40,22]],ellipse(-2,10,7,8),rect(16,29,7,10)],[...radial(-30,0,11,10),...radial(31,0,11,10)]));
put('scooter','Step-through city scooter',mc,[...wheel(-27,0,11),...wheel(28,0,11),poly([[-39,8],[-35,22],[-17,26],[-9,21],[-5,7],[11,7],[14,35],[22,37],[23,20],[33,9],[25,9],[19,2],[-14,2]]),[[-28,27],[-9,27]],[[18,37],[14,42],[7,42]],circle(21,36,4),arc(-27,0,14,.2,2.9)]);
put('touringbike','Touring motorcycle with panniers',mc,moto([poly([[-17,23],[-13,33],[5,35],[16,27],[9,22]]),rect(-35,9,16,17),rect(-38,29,15,10),poly([[17,31],[18,46],[25,44],[26,30]]),ellipse(-1,10,8,8),[[0,1],[22,1]]],[arc(31,0,16,.1,2.9)]));
put('cafebike','Cafe racer with seat cowl',mc,moto([poly([[-30,23],[-30,29],[-23,32],[-14,26],[-9,26],[-5,34],[8,34],[16,25]]),ellipse(0,10,8,7),[[18,28],[24,28]],[[7,4],[26,2]]],[arc(-30,0,15,.2,2.8),circle(23,27,4)]));
put('adventurebike','Adventure motorcycle with luggage',mc,moto([poly([[-19,25],[-9,26],[-4,37],[10,38],[17,30],[6,22]]),rect(-37,13,18,18),poly([[18,33],[19,47],[27,44],[26,31]]),ellipse(0,10,8,7),[[-10,0],[9,-3],[16,5]]],[...radial(-30,0,11,8),...radial(31,0,11,8)]));
put('bicycle','Diamond-frame road bicycle',mc,[...wheel(-31,0,18),...wheel(32,0,18),[[-31,0],[-13,29],[1,0],[-31,0]],[[1,0],[17,29],[-13,29]],[[17,29],[32,0]],[[17,29],[15,37],[24,37],[27,33],[23,31]],[[-13,29],[-16,36]],[[-23,37],[-10,37]],circle(1,0,4),[[1,0],[8,-3]]]);
put('basketbike','Step-through bicycle with front basket',mc,[...wheel(-31,0,18),...wheel(32,0,18),[[-31,0],[-13,26],[1,0],[-31,0]],cubic([-13,26],[-3,4],[5,5],[17,29]),[[1,0],[17,29],[32,0]],[[17,29],[15,38],[23,38]],[[-13,26],[-17,35]],[[-24,35],[-11,35]],poly([[24,38],[43,38],[39,23],[28,23]]),[[30,25],[28,36]],[[36,25],[37,36]],circle(1,0,4)]);
put('tandem','Two-rider tandem bicycle',mc,[...wheel(-51,0,18),...wheel(51,0,18),[[-51,0],[-32,29],[-19,0],[-51,0]],[[ -19,0],[-2,29],[-32,29]],[[-19,0],[13,0],[-2,29]],[[13,0],[34,29],[-2,29]],[[34,29],[51,0]],...[[-32,29],[-2,29]].flatMap(([x,y])=>[[[x,y],[x-3,y+7]],[[x-9,y+7],[x+4,y+7]]]),[[34,29],[31,37],[39,37]],circle(-19,0,4),circle(13,0,4)]);
put('unicycle','Circus unicycle',mc,[...wheel(0,0,22),[[-5,0],[-5,27],[5,27],[5,0]],[[0,27],[0,41]],[[-13,43],[-9,40],[9,40],[13,43]],[[0,0],[12,0],[12,-5]],[[0,0],[-12,0],[-12,5]],...radial(0,0,19,10)]);
const ins='Musical instruments';
const strings=(x0,n,y1,y2,spacing=1)=>Array.from({length:n},(_,i)=>[[x0+i*spacing,y1],[x0+i*spacing,y2]]);
const frets=(x,w,y0,n,gap)=>Array.from({length:n},(_,i)=>[[x,y0+i*gap],[x+w,y0+i*gap]]);
const gbody=close([...cubic([-5,7],[-26,25],[-30,0],[-17,-9]),...cubic([-17,-9],[-38,-36],[38,-36],[17,-9]).slice(1),...cubic([17,-9],[30,0],[26,25],[5,7]).slice(1)]);
put('classicalguitar','Nylon-string classical guitar',ins,[gbody,rect(-4,7,8,41),rect(-6,48,12,13),circle(0,-8,6),circle(0,-8,7.5),rect(-9,-24,18,3),...strings(-2.5,6,-24,57),...frets(-4,8,13,8,4),rect(-3.5,51,2,7),rect(1.5,51,2,7)]);
put('steelguitar','Steel-string dreadnought guitar',ins,[poly([[-5,9],[-22,15],[-27,4],[-20,-8],[-26,-23],[-20,-34],[20,-34],[26,-23],[20,-8],[27,4],[22,15],[5,9]]),rect(-4,9,8,41),poly([[-4,50],[-6,62],[6,62],[4,50]]),circle(0,-7,7),rect(-10,-26,20,3),...strings(-2.5,6,-25,59),...frets(-4,8,13,8,4)]);
put('electricguitar','Double-cutaway electric guitar',ins,[close([...cubic([-4,14],[-12,32],[-19,29],[-17,12]),...cubic([-17,12],[-42,-34],[31,-42],[24,-12]).slice(1),...cubic([24,-12],[12,5],[21,29],[8,21]).slice(1),...cubic([8,21],[6,14],[7,12],[4,14]).slice(1)]),rect(-3,14,6,39),poly([[-3,53],[-5,66],[6,69],[4,53]]),rect(-8,3,16,4),rect(-8,-7,16,4),rect(-8,-19,16,3),...strings(-2,6,-18,62,.8),circle(15,-13,2),circle(15,-22,2)]);
put('bassguitar','Four-string electric bass',ins,[close([...cubic([-4,10],[-12,36],[-24,30],[-18,4]),...cubic([-18,4],[-43,-29],[34,-40],[24,-11]).slice(1),...cubic([24,-11],[9,6],[20,23],[9,24]).slice(1),...cubic([9,24],[7,15],[5,11],[4,10]).slice(1)]),rect(-3,10,6,52),poly([[-3,62],[-3,78],[6,73],[3,62]]),...strings(-2,4,-22,72,1.3),rect(-7,-7,14,4),rect(-6,-23,12,3),circle(16,-12,2),circle(16,-19,2)]);
put('ukulele','Four-string soprano ukulele',ins,[...transform([gbody],0,0,0,.7),rect(-3,5,6,24),rect(-4,29,8,10),circle(0,-5,4),rect(-6,-17,12,2),...strings(-1.8,4,-17,36,1.2),...frets(-3,6,9,5,3)]);
put('banjo','Five-string resonator banjo',ins,[circle(0,-6,22),circle(0,-6,19),rect(-3,16,6,39),rect(-5,55,10,11),rect(-6,-17,12,2),...strings(-2,5,-17,62),...frets(-3,6,21,7,4),...Array.from({length:12},(_,i)=>transform([[[20,0],[23,0]]],0,-6,i*TAU/12)).flat(),circle(-5,37,2)]);
put('mandolin','Teardrop mandolin',ins,[close([...cubic([0,18],[-4,7],[-29,-3],[-19,-21]),...cubic([-19,-21],[-10,-36],[15,-36],[22,-19]).slice(1),...cubic([22,-19],[30,-3],[5,7],[0,18]).slice(1)]),rect(-2.5,17,5,27),rect(-4,44,8,12),ellipse(0,-6,4,6),...strings(-1.75,8,-22,52,.5),[[-7,-23],[7,-23]]]);
const violinBody=close([...cubic([-4,18],[-21,28],[-24,8],[-13,7]),...cubic([-13,7],[-5,3],[-7,-3],[-17,-5]).slice(1),...cubic([-17,-5],[-30,-31],[29,-31],[17,-5]).slice(1),...cubic([17,-5],[7,-3],[5,3],[13,7]).slice(1),...cubic([13,7],[24,8],[21,28],[4,18]).slice(1)]);
const fholes=[cubic([-9,-11],[-15,-8],[-5,6],[-10,8]),cubic([9,-11],[15,-8],[5,6],[10,8])];
put('violin','Violin with bow and f holes',ins,[violinBody,rect(-3,18,6,21),ellipse(0,44,4,6),...fholes,...strings(-1.5,4,-16,44),poly([[-4,-16],[4,-16],[2,-24],[-2,-24]]),[[32,-32],[35,49]],[[34,-32],[37,49]]]);
put('cello','Cello with endpin',ins,[...transform([violinBody,...fholes],0,0,0,1.2),rect(-3,22,6,34),ellipse(0,62,4,7),...strings(-1.5,4,-24,63),poly([[-4,-19],[4,-19],[2,-29],[-2,-29]]),[[0,-30],[0,-48]],[[38,-44],[40,61]]]);
put('doublebass','Double bass with sloping shoulders',ins,[close([...cubic([-4,30],[-20,26],[-26,16],[-15,5]),...cubic([-15,5],[-6,0],[-8,-6],[-21,-8]).slice(1),...cubic([-21,-8],[-38,-42],[38,-42],[21,-8]).slice(1),...cubic([21,-8],[8,-6],[6,0],[15,5]).slice(1),...cubic([15,5],[26,16],[20,26],[4,30]).slice(1)]),rect(-3,30,6,33),ellipse(0,70,4,8),...fholes,...strings(-1.5,4,-26,70),[[0,-36],[0,-49]]]);
put('harp','Concert harp with curved neck',ins,[close([...cubic([-29,-34],[-12,-13],[20,19],[25,40]),...cubic([25,40],[6,54],[-17,29],[-29,30]).slice(1),[-29,-34]]),rect(-34,-39,70,5),[[25,40],[30,-34]],...Array.from({length:12},(_,i)=>{const x=-24+i*4;return [[x,-29+i*2.4],[x,31+Math.sin(i/11*Math.PI)*8]];})]);
put('lyre','Ancient seven-string lyre',ins,[close([...cubic([-23,28],[-38,-20],[-24,-32],[0,-33]),...cubic([0,-33],[24,-32],[38,-20],[23,28]).slice(1),[17,28],...cubic([17,28],[25,-14],[17,-20],[0,-22]).slice(1),...cubic([0,-22],[-17,-20],[-25,-14],[-17,28]).slice(1)]),rect(-26,28,52,5),...strings(-15,7,-19,28,5)]);
put('grandpiano','Grand piano from above',ins,[close([[-30,-38],[-30,30],...cubic([-30,30],[-24,52],[17,48],[15,22]).slice(1),...cubic([15,22],[11,4],[35,6],[35,-15]).slice(1),[35,-38]]),rect(-30,-38,65,18),...Array.from({length:13},(_,i)=>[[-25+i*4.3,-38],[-25+i*4.3,-20]]),...[-25,-21,-12,-8,-4,5,9,18,22,26].map(x=>rect(x,-27,2,7)),cubic([-23,26],[-12,39],[8,33],[6,20]),[[-22,-14],[-22,24]]]);
put('uprightpiano','Upright piano front elevation',ins,[rect(-42,-24,84,63),rect(-37,5,74,28),rect(-42,-12,84,15),...Array.from({length:20},(_,i)=>[[-38+i*4,-12],[-38+i*4,3]]),rect(-40,-36,5,12),rect(35,-36,5,12),...[-8,0,8].map(x=>poly([[x,-24],[x+2,-24],[x+2,-31],[x,-33]]))]);
put('accordion','Piano accordion with bellows',ins,[rect(-36,-25,17,50),rect(-19,-22,42,44),rect(23,-25,16,50),...Array.from({length:11},(_,i)=>[[-17+i*3.6,-22],[-17+i*3.6,22]]),...Array.from({length:10},(_,i)=>[[-36,-20+i*4.5],[-19,-20+i*4.5]]),...Array.from({length:4},(_,r)=>Array.from({length:3},(_,c)=>circle(27+c*4,-15+r*10,1,12))).flat(),cubic([-31,25],[-20,44],[24,44],[32,25])]);
put('concertina','Hexagonal concertina',ins,[poly([[-38,-15],[-28,-25],[-16,-15],[-16,15],[-28,25],[-38,15]]),poly([[16,-15],[28,-25],[38,-15],[38,15],[28,25],[16,15]]),rect(-16,-15,32,30),...Array.from({length:8},(_,i)=>[[-14+i*4,-15],[-14+i*4,15]]),...[-1,1].flatMap(s=>[-10,0,10].map(y=>circle(s*29,y,2,16)))]);
put('trumpet','Brass trumpet with three valves',ins,[poly([[-36,8],[17,8],[36,20],[36,-2],[17,3],[-36,3]]),cubic([-20,3],[-34,-22],[19,-22],[19,3]),cubic([-15,3],[-23,-13],[13,-13],[13,3]),...[-10,0,10].flatMap(x=>[rect(x-2,-9,4,23),[[x,14],[x,20]],[[x-4,20],[x+4,20]]]),rect(-43,3,7,5)]);
put('trombone','Slide trombone',ins,[poly([[5,19],[32,19],[47,30],[47,8],[32,14],[5,14]]),[[6,14],[-34,14]],cubic([-34,14],[-48,14],[-48,1],[-34,1]),[[-34,1],[27,1]],[[25,1],[25,9],[8,9],[8,14]],[[-12,1],[-12,14]],[[0,1],[0,14]],rect(-2,19,7,5)]);
put('frenchhorn','Coiled French horn',ins,[circle(0,0,23),circle(0,0,17),circle(0,0,11),poly([[15,15],[28,23],[35,35],[44,17],[27,13],[22,5]]),cubic([-20,10],[-31,24],[-22,37],[-7,37]),rect(-7,35,6,4),...[-7,0,7].map(x=>rect(x-1.5,-7,3,15))]);
put('tuba','Concert tuba',ins,[close([...cubic([-22,20],[-35,-43],[35,-43],[24,20]),[17,36],[7,36],...cubic([7,36],[11,-16],[-15,-18],[-13,18]).slice(1),[-22,20]]),poly([[7,36],[0,48],[33,48],[17,36]]),...[-9,0,9].flatMap(x=>[rect(x-2,-11,4,33),[[x-4,24],[x+4,24]]]),cubic([-15,11],[-28,32],[-23,39],[-12,39])]);
put('saxophone','Alto saxophone with keywork',ins,[close([...cubic([-10,37],[7,32],[-16,-22],[2,-28]),...cubic([2,-28],[29,-41],[31,-2],[28,2]).slice(1),[17,2],...cubic([17,2],[23,-17],[12,-26],[5,-19]).slice(1),...cubic([5,-19],[-1,-9],[15,37],[-3,43]).slice(1),[-10,43]]),ellipse(23,4,9,4),[[-10,40],[-20,40]],...Array.from({length:7},(_,i)=>circle(1-i*.8,25-i*6,1.8,16))]);
put('clarinet','Clarinet with rings and keys',ins,[poly([[-3,40],[3,40],[5,-29],[12,-40],[-12,-40],[-5,-29]]),poly([[-3,40],[-4,52],[2,58],[3,40]]),...[-24,-8,9,29].map(y=>[[-4,y],[4,y]]),...Array.from({length:9},(_,i)=>circle(0,32-i*6,1.7,16)),[[5,16],[9,15],[9,-15],[5,-17]]]);
put('flute','Transverse concert flute',ins,[rect(-46,-3,92,6),ellipse(-32,0,4,2),...Array.from({length:12},(_,i)=>circle(-17+i*4.7,0,2,16)),...[-25,18,37].map(x=>[[x,-4],[x,4]]),[[0,4],[0,7],[7,7]]]);
put('recorder','Wooden descant recorder',ins,[poly([[-4,34],[-5,46],[5,46],[4,34],[3,20],[5,-36],[9,-43],[-9,-43],[-5,-36],[-3,20]]),rect(-2,31,4,6),...Array.from({length:7},(_,i)=>circle(0,22-i*8,1.6,16)),[[-6,-38],[6,-38]]]);
put('panpipes','Andean panpipe set',ins,[...Array.from({length:10},(_,i)=>rect(-30+i*6,-34+i*5,5,65-i*5)),[[-31,20],[30,20]],[[-31,14],[30,14]],...Array.from({length:10},(_,i)=>ellipse(-27.5+i*6,31,2.5,1))]);
put('bagpipes','Highland bagpipes',ins,[ellipse(0,-5,25,17),poly([[-17,6],[-29,45],[-25,47],[-12,8]]),poly([[-8,10],[-5,57],[0,57],[-3,10]]),poly([[5,10],[15,48],[19,46],[10,8]]),poly([[19,-8],[36,-40],[32,-43],[14,-12]]),[[12,5],[26,24],[29,24]],...[-1,0,1].map(i=>circle(26+i*3,-27+i*5,1,12))]);
put('drumkit','Five-piece acoustic drum kit',ins,[circle(0,-17,19),circle(0,-17,15),ellipse(-18,12,13,6),rect(-31,-1,26,13),ellipse(17,12,13,6),rect(4,-1,26,13),ellipse(-37,-15,12,5),rect(-49,-29,24,14),ellipse(37,-11,13,5),rect(24,-29,26,18),ellipse(-36,36,20,3),ellipse(36,36,20,3),...[-36,36].flatMap(x=>[[[x,35],[x,-35]],[[x-9,-39],[x,-30],[x+9,-39]]]),[[-15,-31],[-22,-40]],[[15,-31],[22,-40]]]);
put('snaredrum','Snare drum and crossed sticks',ins,[ellipse(0,14,29,9),ellipse(0,-10,29,9),[[-29,14],[-29,-10]],[[29,14],[29,-10]],...[-23,-12,0,12,23].map(x=>[[x,6],[x,-15+Math.abs(x)*.2]]),[[-31,30],[28,42]],[[-28,43],[30,27]],ellipse(0,14,25,6)]);
put('djembe','Rope-tuned djembe drum',ins,[ellipse(0,31,24,7),poly([[-24,31],[-18,1],[-7,-11],[-13,-37],[13,-37],[7,-11],[18,1],[24,31]]),...[-18,-9,0,9,18].map(x=>[[x,25],[x*.65,1],[x*.3,-10]]),[[-11,-29],[11,-29]],ellipse(0,-37,13,3)]);
put('tambourine','Tambourine with paired jingles',ins,[circle(0,0,32),circle(0,0,24),...Array.from({length:8},(_,i)=>transform([ellipse(28,0,3,6),[[25,0],[31,0]]],0,0,i*TAU/8)).flat()]);
put('xylophone','Two-mallet concert xylophone',ins,[...Array.from({length:10},(_,i)=>rect(-34+i*7,-27+i*2,6,54-i*4)),[[-35,-8],[36,-8]],[[-35,8],[36,8]],[[-30,39],[9,28]],circle(-32,40,3),[[30,39],[-9,28]],circle(32,40,3)]);
put('maracas','Paired decorated maracas',ins,[...[-1,1].flatMap(s=>transform([ellipse(0,10,12,19),rect(-3,-36,6,28),[[-11,12],[11,12]],[[-11,6],[11,6]],...[-6,0,6].map(x=>circle(x,17,1.5,12))],s*17,0,-s*.3))]);
put('handbell','Tuned handbell with clapper',ins,[close([...cubic([-5,20],[-20,18],[-13,-9],[-28,-18]),[28,-18],...cubic([28,-18],[13,-9],[20,18],[5,20]).slice(1)]),rect(-4,20,8,24),ellipse(0,-18,28,4),[[0,3],[0,-20]],circle(0,-24,5)]);
const nc='Music notation';
const head=(x=0,y=0,filled=true)=>[ellipse(x,y,6,4),...(filled?[-2,0,2].map(d=>[[x-4,y+d],[x+4,y+d]]):[])];
const stemNote=(x=0,y=0,flags=0,hollow=false)=>[...head(x,y,!hollow),[[x+6,y],[x+6,y+32]],...Array.from({length:flags},(_,i)=>cubic([x+6,y+32-i*7],[x+24,y+24-i*7],[x+19,y+18-i*7],[x+14,y+16-i*7]))];
put('whole-note','Semibreve whole note',nc,[ellipse(0,0,9,5),ellipse(0,0,5,3)]);
put('half-note','Minim half note',nc,stemNote(0,0,0,true));
put('quarter-note','Crotchet quarter note',nc,stemNote());
put('eighth-note','Quaver eighth note with curved flag',nc,stemNote(0,0,1));
put('sixteenth-note','Semiquaver sixteenth note',nc,stemNote(0,0,2));
put('thirtysecond-note','Demisemiquaver thirty-second note',nc,stemNote(0,0,3));
put('dotted-quarter','Dotted crotchet',nc,[...stemNote(),circle(14,1,1.8,16)]);
put('dotted-half','Dotted minim',nc,[...stemNote(0,0,0,true),circle(14,1,1.8,16)]);
for(const [id,name,count,beams] of [['pair-eighth','Pair of beamed quavers',2,1],['four-eighth','Four rising beamed quavers',4,1],['pair-sixteenth','Pair of beamed semiquavers',2,2],['four-sixteenth','Four alternating semiquavers',4,2]]){
const p=[];for(let i=0;i<count;i++){const y=beams===1?i*4:(i%2)*6;p.push(...head(i*20,y),[[i*20+6,y],[i*20+6,38]]);}for(let j=0;j<beams;j++)p.push(rect(6,35-j*6,(count-1)*20,3));put(id,name,nc,p);}
put('sharp','Sharp accidental',nc,[[[-6,-18],[-6,18]],[[5,-18],[5,18]],poly([[-13,3],[13,8],[13,12],[-13,7]]),poly([[-13,-10],[13,-5],[13,-1],[-13,-6]])]);
put('flat','Flat accidental',nc,[[[-4,-14],[-4,24]],cubic([-4,3],[14,16],[15,-3],[-4,-14])]);
put('natural','Natural accidental',nc,[[[-6,-8],[-6,23]],[[6,-23],[6,8]],poly([[-6,4],[6,8],[6,4],[-6,0]]),poly([[-6,-8],[6,-4],[6,-8],[-6,-12]])]);
put('double-sharp','Double sharp accidental',nc,[poly([[-12,-12],[-12,-5],[-8,-5],[-3,0],[-8,5],[-12,5],[-12,12],[-5,12],[-5,8],[0,3],[5,8],[5,12],[12,12],[12,5],[8,5],[3,0],[8,-5],[12,-5],[12,-12],[5,-12],[5,-8],[0,-3],[-5,-8],[-5,-12]])]);
put('whole-rest','Semibreve rest on staff line',nc,[[[-24,4],[24,4]],rect(-9,-4,18,8),...[ -2,0,2].map(y=>[[-9,y],[9,y]])]);
put('half-rest','Minim rest on staff line',nc,[[[-24,0],[24,0]],rect(-9,0,18,8),...[2,4,6].map(y=>[[-9,y],[9,y]])]);
put('quarter-rest','Crotchet rest',nc,[poly([[-4,23],[8,10],[-1,3],[8,-9],[-1,-7],[-8,-13],[-4,-23],[-12,-13],[-9,-4],[-2,0],[-10,12],[0,13]])]);
put('eighth-rest','Quaver rest',nc,[circle(-4,10,4),cubic([-1,9],[6,5],[9,15],[10,17]),[[10,17],[0,-16]]]);
put('sixteenth-rest','Semiquaver rest',nc,[circle(-6,16,4),circle(-2,3,4),cubic([-3,15],[6,11],[12,18],[13,23]),cubic([1,2],[6,0],[8,6],[10,10]),[[13,23],[0,-19]]]);
put('bass-clef','Bass F clef',nc,[cubic([-14,10],[-7,36],[28,23],[6,0]),cubic([6,0],[0,-8],[-7,-13],[-15,-18]),circle(-11,11,4),circle(20,17,2.4),circle(20,5,2.4)]);
put('treble-clef','Treble G clef',nc,[cubic([5,-33],[23,-43],[19,-16],[9,13]),cubic([9,13],[-2,35],[1,45],[9,46]),cubic([9,46],[24,32],[-23,11],[-17,-4]),cubic([-17,-4],[-10,-25],[23,-18],[17,0]),cubic([17,0],[12,15],[-9,11],[-7,0]),cubic([-7,0],[-6,-5],[-1,-7],[3,-5]),circle(5,-34,3)]);
put('alto-clef','Alto C clef',nc,[rect(-20,-24,3,48),rect(-13,-24,3,48),poly([[-7,0],[0,8],[0,-8]]),cubic([0,8],[5,35],[28,28],[22,10]),cubic([22,10],[18,2],[8,4],[2,0]),cubic([2,0],[8,-4],[18,-2],[22,-10]),cubic([22,-10],[28,-28],[5,-35],[0,-8])]);
put('fermata','Fermata pause sign',nc,[arc(0,0,21,0,Math.PI),circle(0,4,3)]);
put('crescendo','Crescendo hairpin',nc,[[[28,12],[-28,0],[28,-12]]]);
put('diminuendo','Diminuendo hairpin',nc,[[[-28,12],[28,0],[-28,-12]]]);
put('repeat-bars','Repeat bar lines with dots',nc,[rect(7,-22,4,44),[[0,-22],[0,22]],circle(-8,6,2),circle(-8,-6,2),...[-20,-10,0,10,20].map(y=>[[-20,y],[15,y]])]);
put('coda','Coda navigation symbol',nc,[circle(0,0,17),[[-26,0],[26,0]],[[0,-26],[0,26]]]);
put('segno','Segno navigation symbol',nc,[cubic([10,18],[-15,30],[-24,6],[0,0]),cubic([0,0],[24,-6],[15,-30],[-10,-18]),[[-24,-22],[24,22]],circle(-17,6,2.5),circle(17,-6,2.5)]);
})();
});

// Original expanded garden collection. Botanical subjects are stylised engraving art.
(function(){
const put=(id,name,category,p)=>add('millennium-garden-'+id,name,category,p);
const petal=(length,width,shape=0)=>close([...cubic([0,0],[length*.15,width],[length*.78,width*(1+shape*.13)],[length,0],28),...cubic([length,0],[length*.78,-width*(1-shape*.08)],[length*.15,-width],[0,0],28).slice(1)]);
function gardenBloom(n){const kind=n%10,v=Math.floor(n/10),p=[];if(kind===0){for(let ring=0;ring<4;ring++){const radius=7+ring*8,petals=5+ring+v;for(let j=0;j<petals;j++){const a=j*TAU/petals+ring*.39;const shape=close([...arc(0,0,radius,a,a+TAU/petals,18),...cubic([radius*Math.cos(a+TAU/petals),radius*Math.sin(a+TAU/petals)],[(radius-8)*Math.cos(a+.8),(radius-8)*Math.sin(a+.8)],[(radius-8)*Math.cos(a+.2),(radius-8)*Math.sin(a+.2)],[radius*Math.cos(a),radius*Math.sin(a)],20)]);p.push(shape);}}return p;}if(kind===1){p.push(close([...cubic([-25-v*2,23],[-26,-26],[26,-26],[25+v*2,23]),[11,14],[0,30+v*3],[-11,14]]));p.push(cubic([-11,14],[-13,0],[-7,-9],[0,-15]),cubic([11,14],[13,0],[7,-9],[0,-15]));return p;}if(kind===5){for(let j=0;j<3+v;j++){const x=(j%2?1:-1)*(11+j*2),y=30-j*14;const bell=close([...cubic([-9,9],[-12,-3],[-11,-8],[-15,-13]),...cubic([-15,-13],[-3,-7],[3,-7],[15,-13]).slice(1),...cubic([15,-13],[11,-8],[12,-3],[9,9]).slice(1)]);p.push(cubic([0,40],[x,40],[x,y+10],[x,y+8]),...transform([bell],x,y,0,.7));}return p;}if(kind===8){for(let row=0;row<4;row++)for(let col=0;col<4;col++){const x=(col-1.5)*14+(row%2)*6,y=(row-1.5)*13;for(let j=0;j<4;j++)p.push(...transform([petal(8+v,4,j%2)],x,y,j*Math.PI/2+.2));p.push(circle(x,y,1,12));}return p;}const count=[0,0,4+v,6,5,0,15+v*3,3,0,8+v*2][kind];let out=[];for(let ring=0;ring<(kind===9?2:1);ring++)for(let j=0;j<count;j++){const a=j*TAU/count+ring*Math.PI/count,len=kind===3?36:kind===7?39:32+v*2,w=kind===2?24:kind===3?8:kind===4?13:kind===7?18:8;out=overlay(out,transform([petal(len-ring*7,w,n%3)],0,0,a));if(kind===3||kind===4||kind===7)out.push(...transform([[[5,0],[len*.78,0]]],0,0,a));}const centre=kind===6?12:kind===2?7:4;out=overlay(out,[circle(0,0,centre,36)]);if(kind===6||kind===2)for(let j=0;j<16+v*3;j++){const a=j*2.4,r=centre*.8*Math.sqrt(j/(16+v*3));out.push(circle(r*Math.cos(a),r*Math.sin(a),.6,10));}return out;}
const flowerNames=['Garden rose','Spring tulip','Field poppy','Oriental lily','Orchid spray','Bluebell cluster','Sunflower','Bearded iris','Hydrangea head','Water lotus','Wild rose','Parrot tulip','California poppy','Tiger lily','Moth orchid','Foxglove bells','Autumn sunflower','Siberian iris','Lacecap hydrangea','Sacred lotus','Floribunda rose','Fringed tulip','Iceland poppy','Asiatic lily','Cattleya orchid','Lily of the valley','Dwarf sunflower','Dutch iris','Oakleaf hydrangea','Pond lotus'];
for(let n=0;n<30;n++){
    register('garden-flowers-' + n, () => {
const head=gardenBloom(n);put('flower-'+n,flowerNames[n]+' · blossom study','Garden flowers',head);let spray=[cubic([0,-70],[10,-42],[-8,-20],[0,0])];for(let j=0;j<3;j++)spray=overlay(spray,transform(botanicalLeaf(26-j*3,6+n%4,j+n),j%2?3:-2,-55+j*15,j%2?.6:2.55));for(const q of head)if(q.length>3&&q[0][0]===q.at(-1)[0]&&q[0][1]===q.at(-1)[1])spray=ornaments.clip(spray,q);spray.push(...head);put('spray-'+n,flowerNames[n]+' · leafy stem','Floral sprays',spray);const corner=[...transform(head,0,0,0,.5),...transform(spray,20,0,-Math.PI/2,.55),...transform(spray,0,20,0,.55)];put('corner-'+n,flowerNames[n]+' · garden corner','Floral corners',corner);
    });
}
const succulentNames=['Echeveria','Haworthia','Aloe','Agave','Jade plant','Burro tail','Aeonium','Sempervivum','Zebra succulent','Paddle plant','Barrel cactus','Prickly pear','Saguaro','Moon cactus','Bunny ears cactus','Star cactus','Fishbone cactus','Pincushion cactus','Organ pipe cactus','Christmas cactus'];
for(let n=0;n<20;n++){
    register('succulents-' + n, () => {
let p=[];if(n<10){const count=5+n,kind=n%3;for(let ring=0;ring<2;ring++)for(let i=0;i<count;i++){const a=i*TAU/count+ring*.23,len=28-ring*9+n*.3,w=kind===0?8:kind===1?4:11;p=overlay(p,transform([petal(len,w,n%3)],0,12,a));}p.push(rect(-24,-29,48,5),poly([[-21,-29],[21,-29],[15,-52],[-15,-52]]));for(let i=0;i<3;i++)p.push([[-13+i*12,-34],[-10+i*10,-47]]);}else if(n===11||n===14){for(const[x,y,rx,ry]of [[0,2,12,24],[-13,23,9,15],[14,27,10,17]]){const shape=circle(0,0,1,48).map(([a,b])=>[x+a*rx,y+b*ry]);p=overlay(p,[shape]);for(let j=0;j<5;j++)p.push([[x-2,y-10+j*5],[x+2,y-8+j*5]]);}p.push(poly([[-18,-23],[18,-23],[12,-44],[-12,-44]]));}else{const count=n===12||n===18?3:1;for(let j=0;j<count;j++){const x=(j-(count-1)/2)*17,h=40-(j%2)*15+n%4*3;p.push(close([...arc(x,h,9,0,Math.PI,24),[x-9,-14],[x+9,-14]]));for(const dx of [-4,0,4])p.push([[x+dx,-10],[x+dx,h+5]]);for(let k=0;k<6;k++)p.push([[x-12,k*6-6],[x-8,k*6-4]],[[x+8,k*6-4],[x+12,k*6-6]]);}p.push(rect(-29,-19,58,5),poly([[-25,-19],[25,-19],[19,-42],[-19,-42]]));}if(n===14)for(const [x,y]of [[0,2],[-13,23],[14,27]])for(let j=0;j<5;j++)p.push(circle(x+(j%2?3:-3),y-7+j*4,.7,10));
if(n===17){p=[circle(0,11,28,64),rect(-25,-22,50,5),poly([[-21,-22],[21,-22],[15,-43],[-15,-43]])];for(let j=0;j<24;j++){const a=j*2.4,r=24*Math.sqrt(j/24),x=r*Math.cos(a),y=11+r*Math.sin(a);p.push([[x-2,y],[x+2,y]],[[x,y-2],[x,y+2]]);}}
if(n===19||n===16){p=[poly([[-23,-20],[23,-20],[17,-42],[-17,-42]])];for(let side of [-1,1])for(let j=0;j<5;j++){const x=side*j*9,y=8+j*7-j*j*1.9;p.push(...transform([petal(16,n===16?5:7,j%3)],x,y,side===1?.6-j*.3:Math.PI-.6+j*.3));if(n===19&&j===4)p.push(...transform([petal(14,4,0),petal(10,7,1)],x+side*9,y-6,-Math.PI/2));}}
put('succulent-'+n,succulentNames[n]+' · ceramic planter','Succulents & cacti',p);
    });
}
const herbNames=['Rosemary','Thyme','Basil','Mint','Sage','Oregano','Marjoram','Lavender','Dill','Fennel','Parsley','Coriander','Chives','Tarragon','Lemon balm','Chamomile','Anise','Bay herb','Sorrel','Stevia'];
for(let n=0;n<20;n++){
    register('herbs-' + n, () => {
let p=[];const stalks=2+n%3;for(let s=0;s<stalks;s++){const x=(s-(stalks-1)/2)*12,h=60+s*7+n%4*3;p.push(cubic([0,0],[x*.5,16],[x,40],[x,h]));if(n===8||n===9||n===16){for(let j=0;j<9;j++){const a=Math.PI*.12+j*Math.PI*.095;p.push([[x,h-12],[x+22*Math.cos(a),h-12+22*Math.sin(a)]]);p.push(circle(x+22*Math.cos(a),h-12+22*Math.sin(a),1.6,12));}}else for(let j=0;j<6;j++){const y=12+j*(h-12)/6;for(const side of [-1,1]){const leafLen=n%5===0?14:n%5===1?7:11,width=n%5===0?1.5:3+n%3;p=overlay(p,transform(botanicalLeaf(leafLen,width,n+j),x*y/h,y,side===1?.45:Math.PI-.45));}}if(n===7)for(let j=0;j<5;j++)for(const side of [-1,1])p.push(...transform([petal(6,2,0)],x,h+j*4,side===1?.8:Math.PI-.8));}put('herb-'+n,herbNames[n]+' · kitchen herb study','Herbs & spices',p);
    });
}
const fruitNames=['Pear','Peach','Orange','Lemon','Strawberry','Cherry pair','Pomegranate','Fig','Pineapple','Avocado'];
function fruit(n){let p=[];if(n===0)p.push(close([...cubic([0,30],[-9,29],[-7,5],[-19,-6]),...cubic([-19,-6],[-32,-37],[31,-37],[19,-6]).slice(1),...cubic([19,-6],[7,5],[9,29],[0,30]).slice(1)]));else if(n===3)p.push(close(Array.from({length:80},(_,i)=>{const a=i*TAU/80;return[33*Math.cos(a),18*Math.sin(a)*(1+.15*Math.cos(2*a))];})));else if(n===4){p.push(close([...cubic([-24,15],[-29,-1],[-7,-31],[0,-34]),...cubic([0,-34],[7,-31],[29,-1],[24,15]).slice(1),...cubic([24,15],[9,24],[-9,24],[-24,15]).slice(1)]));for(let row=0;row<4;row++)for(let c=0;c<5-row;c++)p.push(circle((c-(4-row)/2)*7,12-row*10,1,12));}else if(n===5){p.push(circle(-14,-8,14),circle(14,-8,14),cubic([-14,6],[-10,22],[2,32],[5,33]),cubic([14,6],[17,25],[9,29],[5,33]));}else if(n===8){p.push(close(Array.from({length:72},(_,i)=>[21*Math.cos(i*TAU/72),-7+29*Math.sin(i*TAU/72)])));for(let j=-2;j<=2;j++){p.push([[j*7-10,-27],[j*7+10,13]],[[j*7-10,13],[j*7+10,-27]]);}for(let j=0;j<5;j++)p.push(...transform([petal(27,4,j%3)],0,21,Math.PI*.25+j*Math.PI*.125));}else if(n===9){p.push(close([...cubic([0,33],[-12,34],[-32,-18],[-18,-29]),...cubic([-18,-29],[-8,-40],[8,-40],[18,-29]).slice(1),...cubic([18,-29],[32,-18],[12,34],[0,33]).slice(1)]),circle(0,-13,11));}else{p.push(circle(0,0,26,72).map(([x,y])=>[x,y*(n===7?1.2:1)]));if(n===1)p.push(cubic([2,24],[-13,14],[10,-14],[0,-25]));if(n===2)for(let j=0;j<22;j++){const a=j*2.4,r=21*Math.sqrt(j/22);p.push(circle(r*Math.cos(a),r*Math.sin(a),.55,8));}if(n===6)p.push(poly([[-8,25],[-10,36],[-3,32],[0,38],[3,32],[10,36],[8,25]]));if(n===7)p.push(cubic([0,29],[-5,10],[2,-9],[0,-28]));}if(![3,5,8,9].includes(n)){p.push(cubic([0,25],[1,34],[6,38],[9,39]));p.push(...transform([petal(18,5,n%3)],4,31,.4));}return p;}
for(let n=0;n<10;n++){
    register('orchard-' + n, () => {
const f=fruit(n);put('fruit-'+n,fruitNames[n]+' · fruit engraving','Fruit & orchard',f);let b=[cubic([-46,-12],[-19,16],[15,4],[47,28])];for(let j=0;j<4;j++)b=overlay(b,transform(botanicalLeaf(17+j*2,5,n),-34+j*22,j*8,j%2?.7:2.5));b.push(...transform(f,9,-27,0,.6));put('fruit-branch-'+n,fruitNames[n]+' · orchard branch','Fruit & orchard',b);
    });
}
for(let n=0;n<20;n++){
    register('fungi-' + n, () => {
let p=[];const group=1+Math.floor(n/5),type=n%5;for(let j=0;j<group;j++){const x=(j-(group-1)/2)*28,y=(j%2)*-8,r=19+(n+j)%4*2,h=type===0?19:type===1?10:type===2?29:type===3?15:24;const cap=close([...cubic([x-r,y],[x-r*.8,y+h],[x+r*.8,y+h],[x+r,y]),...cubic([x+r,y],[x+r*.4,y-4],[x-r*.4,y-4],[x-r,y]).slice(1)]);const stem=poly([[x-4,y],[x+4,y],[x+7,y-29],[x-6,y-29]]);p=overlay(p,[stem]);p=overlay(p,[cap]);if(type===0||type===3)for(let k=0;k<5;k++)p.push(circle(x-12+k*6,y+5+Math.sin(k)*4,1.6,12));else for(let k=0;k<7;k++)p.push(cubic([x-r+3+k*(2*r-6)/6,y+1],[x-6+k*2,y+3],[x-3+k,y+6],[x,y+h-2]));}p.push(cubic([-group*16,-33],[-15,-26],[20,-38],[group*16,-31]));put('fungi-'+n,['Spotted toadstool','Chanterelle','Morel-inspired cap','Woodland parasol','Inkcap'][type]+' · cluster '+group,'Mushrooms & fungi',p);
    });
}
for(let n=0;n<25;n++){
    register('dividers-' + n, () => {
let p=[];const kind=n%5,repeats=3+Math.floor(n/5);for(let i=0;i<repeats;i++){const x=i*34;if(kind===0)p.push(...transform(gardenBloom((i+n)%30),x,0,0,.32));else if(kind===1)p.push(...transform(botanicalLeaf(25,5,i+n),x-12,0,(i%2?.4:-.4)));else if(kind===2){p.push(arc(x,0,13,0,Math.PI,24),arc(x,0,8,0,Math.PI,24),circle(x,5,1.4,12));}else if(kind===3){p.push(cubic([x-17,0],[x-8,19],[x+8,-19],[x+17,0]),...transform([petal(16,4,i%3)],x,0,i%2?2.4:.7));}else{p.push(...transform([petal(17,4,1)],x-5,0,.9),...transform([petal(17,4,1)],x+5,0,2.2),circle(x,-6,2,14));}}p.push([[-19,-19],[repeats*34-15,-19]],[[-19,-22],[repeats*34-15,-22]]);put('divider-'+n,['Blossom chain','Herb garland','Scalloped garden edging','Trailing leaf scroll','Berry sprig'][kind]+' · divider '+(1+Math.floor(n/5)),'Botanical dividers',p);
    });
}
})();
/* Original tree studies and harvest compositions, generated for CNC Studio. */
(function(){
// Closed foreground contours physically trim earlier vector paths before export.
const pathBoxes=new WeakMap();
const bounds=p=>{if(pathBoxes.has(p))return pathBoxes.get(p);let b=[Infinity,Infinity,-Infinity,-Infinity];for(const [x,y]of p){b[0]=Math.min(b[0],x);b[1]=Math.min(b[1],y);b[2]=Math.max(b[2],x);b[3]=Math.max(b[3],y);}pathBoxes.set(p,b);return b;};
const layered=paths=>paths.reduce((out,path)=>{const a=path[0],b=path[path.length-1];if(path.length<=3||Math.hypot(a[0]-b[0],a[1]-b[1])>=1e-8)return [...out,path];const mask=bounds(path),result=[];for(const prior of out){const q=bounds(prior);if(q[2]<mask[0]||q[0]>mask[2]||q[3]<mask[1]||q[1]>mask[3])result.push(prior);else result.push(...ornaments.clip([prior],path));}return [...result,path];},[]);
const put=(id,name,cat,p)=>add('millennium-tg-'+id,name,cat,cat==='Wheat & grain'?layered(p):p);
const species=[
 ['oak','English oak',0,8],['elm','Arching elm',0,11],['maple','Sugar maple',0,6],['beech','European beech',0,13],['chestnut','Horse chestnut',0,9],
 ['birch','Silver birch',1,6],['aspen','Quaking aspen',1,9],['poplar','Lombardy poplar',1,13],['eucalyptus','River red gum',2,8],['acacia','Acacia',2,12],
 ['pine','Scots pine',3,5],['spruce','Blue spruce',3,8],['fir','Silver fir',3,11],['cedar','Cedar of Lebanon',4,4],['cypress','Italian cypress',4,9],
 ['willow','Weeping willow',5,10],['palm','Date palm',6,9],['coconut','Coconut palm',6,6],['baobab','African baobab',7,7],['dragon','Dragon blood tree',7,12],
 ['apple','Apple orchard tree',8,7],['pear','Pear orchard tree',8,10],['cherry','Flowering cherry',9,7],['magnolia','Magnolia',9,11],['bamboo','Bamboo grove',10,7]
];
function tree(kind,n){
 let p=[];
 if(kind===3){const edge=[[0,76]];for(let j=0;j<n;j++){const y=68-j*52/n,w=7+j*23/n;edge.push([-w,y-8],[-w*.49,y-5]);}edge.push([-3,11],[-3,0],[3,0],[3,11]);for(let j=n-1;j>=0;j--){const y=68-j*52/n,w=7+j*23/n;edge.push([w*.49,y-5],[w,y-8]);}p.push(poly(edge));for(let j=1;j<4;j++)p.push([[-2,2+j*2],[1,3+j*2]]);return p;}
 if(kind===4){const slender=n>6,edge=[[0,76]];for(let j=n-1;j>=0;j--){const y=15+j*50/n,w=slender?10-5*j/n:31-16*j/n;edge.push([-w,y+3],[-w,y],[-w*.36,y-2]);}edge.push([-2,10],[-2,0],[2,0],[2,10]);for(let j=0;j<n;j++){const y=15+j*50/n,w=slender?10-5*j/n:31-16*j/n;edge.push([w*.36,y-2],[w,y],[w,y+3]);}p.push(poly(edge));return p;}
 if(kind===6){p.push(poly([[-4,0],[2,0],[9,50],[6,54]]));for(let j=0;j<n;j++){let a=.12+j*(Math.PI-.24)/(n-1);let end=[7+37*Math.cos(a),48+22*Math.sin(a)];p.push(cubic([7,51],[end[0]*.6,75],[end[0],68],end));for(let k=1;k<6;k++){let t=k/6,x=7+(end[0]-7)*t,y=51+(end[1]-51)*t+15*Math.sin(t*Math.PI);p.push([[x,y],[x+Math.cos(a)*5,y-7]]);}}p.push(circle(5,48,2.5),circle(10,47,2));return p;}
 if(kind===10){for(let j=0;j<5;j++){let x=(j-2)*9,h=45+(j*13%n)*3;p.push([[x-1,0],[x-1,h]],[[x+1,0],[x+1,h]]);for(let y=9;y<h;y+=10){p.push([[x-2,y],[x+2,y]]);p.push(...transform(botanicalLeaf(13,2,j),x,y,(j%2? .5:2.7)));}}return p;}
 const trunk=kind===7?10:kind===1?2:4;
 p.push(poly([[-trunk,0],[trunk,0],[trunk*.6,35],[0,46],[-trunk*.6,35]]));
 let cx=0,cy=kind===1?53:kind===7?56:48,rx=kind===1?14:kind===7?34:32,ry=kind===1?31:kind===7?14:24;
 const canopy=poly(Array.from({length:160},(_,i)=>{let a=i*TAU/160;let f=1+.075*Math.cos(a*n)+.04*Math.sin(a*3);return[cx+rx*Math.cos(a)*f,cy+ry*Math.sin(a)*f];}));
 if(kind!==5)p.push(canopy);
 for(let j=0;j<6;j++){let s=j%2?-1:1,y=21+j*4;let x=s*(15+(j%3)*6);p.push(cubic([0,y],[s*6,y+7],[x,y+9],[x,kind===7?56:49+j*2]));}
 if(kind===5){for(let j=0;j<n+5;j++){let x=-34+j*68/(n+4),top=54+15*Math.sqrt(Math.max(0,1-(x/35)**2));p.push(cubic([0,39],[x*.65,top+9],[x,top],[x,12+Math.abs(x)*.5]));for(let k=0;k<3;k++)p.push(...transform([botanicalLeaf(6,1.1,k)[0]],x,top-12-k*8,-1.45));}}
 if(kind===1)for(let y=6;y<31;y+=5)p.push([[-trunk,y],[trunk*.7,y+1]]);
 if(kind===2){for(let j=0;j<8;j++){let a=j*TAU/8;p.push(...transform([botanicalLeaf(10,2,j)[0]],22*Math.cos(a),48+14*Math.sin(a),a));}}
 if(kind===8)for(let j=0;j<n;j++){let a=j*2.4,r=8+(j%3)*7,x=Math.cos(a)*r,y=48+Math.sin(a)*r*.7;if(n===10)p.push(poly([[x,y+4],[x-1,y+1],[x-3,y-2],[x-2,y-4],[x+2,y-4],[x+3,y-2],[x+1,y+1]]));else p.push(circle(x,y,2.7,18));}
 if(kind===9)for(let j=0;j<n;j++){let a=j*2.4,r=7+j%3*7,x=Math.cos(a)*r,y=49+Math.sin(a)*r*.8;p.push(poly(Array.from({length:50},(_,k)=>{let t=k*TAU/50,r=2.7+.8*Math.cos(t*5);return[x+r*Math.cos(t),y+r*Math.sin(t)];})));}
 return p;
}
for(let i=0;i<species.length;i++){
    register('tree-' + i, () => {

 const[id,name,k,n]=species[i],p=tree(k,n);put(id+'-study',name+' · botanical engraving','Trees',p);
 let scene=[...transform(p,-24,0,0,.8),...transform(tree(k,n),28,5,0,.57)];
 const layout=i%5;
 if(layout===0){scene.push(cubic([-60,-2],[-20,6],[15,-10],[62,0]),cubic([-60,-8],[-10,2],[24,-16],[62,-6]));for(let j=0;j<3;j++)scene.push(cubic([-38+j*5,-12-j*5],[-10,-8-j*4],[10,-12-j*5],[44-j*6,-13-j*5]));}
 if(layout===1){scene.push(cubic([-60,0],[-23,-8],[13,13],[62,0]),rect(-3,3,19,2),rect(-3,9,19,5),[[0,3],[0,-4]],[[13,3],[13,-4]]);}
 if(layout===2){scene.push([[-62,0],[-49,22],[-37,6]],[[4,9],[19,27],[44,5],[61,0]],arc(15,40,9,.15,Math.PI-.15,24),[[-62,-2],[62,-2]]);}
 if(layout===3){scene.push(cubic([-62,-2],[-27,0],[25,-9],[62,0]));for(let j=0;j<7;j++){let x=-52+j*16;scene.push([[x,-2],[x,7]],[[x,-2],[x-3,3]],[[x,-2],[x+3,4]]);}}
 if(layout===4){scene.push(rect(-63,-9,126,1),[[-5,-8],[2,8],[10,8],[18,-8]],[[0,-1],[14,-1]],circle(48,56,6,32));}
 const purposes=['riverbank','garden bench','mountain sunrise','meadow','country gateway'];put(id+'-scene',name+' · '+purposes[layout],'Trees',scene);

    });
}
const grains=[['wheat','Bread wheat',0],['durum','Durum wheat',1],['barley','Awned barley',2],['rye','Rye',3],['oats','Oats',4],['rice','Rice',5],['millet','Pearl millet',6],['sorghum','Sorghum',7],['buckwheat','Buckwheat',8],['quinoa','Quinoa',9]];
function stalk(type){let p=[cubic([0,0],[1,13],[-2,26],[0,38]),[[0,30],[0,54]]];
 for(let j=0;j<3;j++)p.push(...transform([botanicalLeaf(16+j*2,1.5,j)[0]],0,5+j*7,j%2?.55:2.55));
 if(type<4){let rows=type===3?10:7;for(let j=0;j<rows;j++)for(const s of [-1,1]){let y=30+j*2.8,x=s*(type===3?3:4);p.push(...transform([botanicalLeaf(6,1.8,type)[0]],0,y,s===1?.62:2.52));if(type===1||type===2)p.push([[x,y+2],[x+s*(type===2?7:3),y+12]]);}}
 else if(type===4||type===5){for(let j=0;j<8;j++){let s=j%2?-1:1,x=s*(6+j%3*3),y=30+j*3;p.push(cubic([0,y-4],[x*.5,y+3],[x,y+2],[x,y-2]));p.push(...transform([botanicalLeaf(type===4?6:4,1.8,j)[0]],x,y-2,-Math.PI/2));}}
 else if(type<8){for(let j=0;j<9;j++){let y=31+j*2.7,w=(type===6?4:9)*Math.sin((j+1)*Math.PI/11);for(let x=-w;x<=w;x+=2.6)p.push(circle(x,y,1.1,12));}}
 else {for(let j=0;j<7;j++){let s=j%2?-1:1,x=s*(5+j%3*3),y=27+j*3.5;p.push([[0,y-4],[x,y]]);if(type===8){p.push(poly([[x,y+2],[x-2,y-1],[x+2,y-1]]));p.push(...transform([botanicalLeaf(7,2,j)[0]],x,y-2,s===1?.3:2.8));}else for(let k=0;k<5;k++)p.push(circle(x+Math.cos(k*TAU/5)*2,y+Math.sin(k*TAU/5)*2,.9,10));}}
 return layered(p);
}
for(const [id,name,type]of grains){
    register('grain-' + id, () => {
const p=stalk(type);
 put(id+'-study',name+' · botanical ear study','Wheat & grain',p);
 let sheaf=[];for(let j=-2;j<=2;j++)sheaf.push(...transform(p,0,0,j*.16,1-Math.abs(j)*.07));sheaf.push(poly([[-8,10],[8,12],[8,16],[-8,14]]),cubic([0,12],[-17,8],[-13,23],[0,14]),cubic([0,12],[17,8],[13,23],[0,14]));put(id+'-sheaf',name+' · tied harvest sheaf','Wheat & grain',sheaf);
 let wreath=[];for(let s of [-1,1])for(let j=0;j<4;j++)wreath.push(...transform(p,s*(25+3*Math.sin(j)),5+j*13,s*(.52-j*.1),.55));wreath.push(cubic([-25,8],[-20,-4],[20,-4],[25,8]));put(id+'-wreath',name+' · open harvest wreath','Wheat & grain',wreath);
 let field=[];for(let j=0;j<9;j++)field.push(...transform(p,-44+j*11,(j%3)*2,(j%3-1)*.1,.57+(j%3)*.08));field.push(cubic([-53,-3],[-20,1],[20,-6],[53,-3]),arc(25,45,10,0,Math.PI,24));put(id+'-field',name+' · sunrise field border','Wheat & grain',field);
 let harvest=[...transform(p,-23,0,-.3,.9),...transform(p,23,0,.3,.9),poly([[-24,-3],[24,-3],[17,-18],[-17,-18]])];for(let y=-7;y>-18;y-=4)harvest.push([[-21,y],[21,y]]);for(let x=-12;x<=12;x+=6)harvest.push([[x,-3],[x*.7,-18]]);harvest.push(arc(0,-2,17,0,Math.PI,24));put(id+'-basket',name+' · harvest basket emblem','Wheat & grain',harvest);

    });
}
})();

function buildGroup(key) {
    const recipe = recipes.get(key);
    if (!recipe) throw new Error('Unknown template recipe: ' + key);
    output = [];
    try {
        recipe();
        return output;
    } finally {
        output = null;
    }
}

// scripts/build_templates.cjs uses this entry point to generate metadata from the recipes.
function buildCatalog() {
    const index = [], templates = [];
    for (const key of recipes.keys()) {
        for (const template of buildGroup(key)) {
            templates.push(template);
            index.push([key, template.id, template.name, template.category, template.aspect]);
        }
    }
    return {index, templates};
}

// Bound retained geometry independently of the number of categories the operator visits.
const cache = new Map();
const maxCachedPoints = 100000;
let cachedPoints = 0;
function getPaths(key, id) {
    let group = cache.get(key);
    if (!group) {
        const geometry = new Map();
        let count = 0;
        for (const item of buildGroup(key)) {
            geometry.set(item.id, item.paths);
            for (const path of item.paths) count += path.length;
        }
        group = {geometry, count};
        while (cache.size && cachedPoints + count > maxCachedPoints) {
            const oldest = cache.keys().next().value;
            cachedPoints -= cache.get(oldest).count;
            cache.delete(oldest);
        }
        cachedPoints += count;
    }
    cache.delete(key);
    cache.set(key, group);
    const paths = group.geometry.get(id);
    if (!paths) throw new Error('Template index needs rebuilding for ' + id);
    return paths;
}

// BEGIN GENERATED TEMPLATE INDEX
const templateIndex = [
["artisan","artisan-vine-0","Olive botanical branch","Botanical vines",2.8172322673985075],
["artisan","artisan-corner-0","Olive engraved corner","Botanical corners",1.0562440803765636],
["artisan","artisan-frame-0","Olive botanical frame","Botanical frames",1.3605324996458883],
["artisan","artisan-swirl-0","Palmette ornament","Ornamental swirls",3.285429049605041],
["artisan","artisan-vine-1","Oak botanical branch","Botanical vines",2.6930457374942263],
["artisan","artisan-corner-1","Oak engraved corner","Botanical corners",1.0558450059514943],
["artisan","artisan-frame-1","Oak botanical frame","Botanical frames",1.3583122414949238],
["artisan","artisan-swirl-1","Double scroll ornament","Ornamental swirls",3.5951685133814837],
["artisan","artisan-vine-2","Tendril botanical branch","Botanical vines",2.8172322673985075],
["artisan","artisan-corner-2","Tendril engraved corner","Botanical corners",1.0562440803765636],
["artisan","artisan-frame-2","Tendril botanical frame","Botanical frames",1.3605324996458883],
["artisan","artisan-swirl-2","Acanthus ornament","Ornamental swirls",2.887296840153983],
["artisan","artisan-grape-vine","Vintner grapevine with clusters","Botanical vines",2.0338813369886024],
["artisan","artisan-grapes","Grape cluster · engraved highlights","Botanical vines",0.75],
["artisan","artisan-laurel","Classical laurel wreath","Wreaths",1.2358455030869486],
["artisan","artisan-olive-wreath","Olive wreath","Wreaths",1.229250524014855],
["classic","flower-round-4","4-petal rounded flower","Flowers",1],
["classic","flower-point-4","4-petal pointed flower","Flowers",1],
["classic","flower-round-5","5-petal rounded flower","Flowers",0.9636730947397327],
["classic","flower-point-5","5-petal pointed flower","Flowers",0.9582269344573815],
["classic","flower-round-6","6-petal rounded flower","Flowers",1.125045525780579],
["classic","flower-point-6","6-petal pointed flower","Flowers",1.1493286700487992],
["classic","flower-round-7","7-petal rounded flower","Flowers",0.9781734011163361],
["classic","flower-point-7","7-petal pointed flower","Flowers",0.9749279121818234],
["classic","flower-round-8","8-petal rounded flower","Flowers",1],
["classic","flower-point-8","8-petal pointed flower","Flowers",1],
["classic","flower-round-10","10-petal rounded flower","Flowers",1.0480166108348077],
["classic","flower-point-10","10-petal pointed flower","Flowers",1.0514622242382672],
["classic","flower-round-12","12-petal rounded flower","Flowers",1],
["classic","flower-point-12","12-petal pointed flower","Flowers",1],
["classic","flower-round-16","16-petal rounded flower","Flowers",1],
["classic","flower-point-16","16-petal pointed flower","Flowers",1],
["classic","vine-straight-3","Upright vine · 3 leaves","Vines",0.4280629126532163],
["classic","vine-wave-3","Winding vine · 3 leaves","Vines",0.5613962459865496],
["classic","vine-straight-4","Upright vine · 4 leaves","Vines",0.4280629126532163],
["classic","vine-wave-4","Winding vine · 4 leaves","Vines",0.6816779836652572],
["classic","vine-straight-5","Upright vine · 5 leaves","Vines",0.42365874583074065],
["classic","vine-wave-5","Winding vine · 5 leaves","Vines",0.6522228037948774],
["classic","vine-straight-6","Upright vine · 6 leaves","Vines",0.41390525184293636],
["classic","vine-wave-6","Winding vine · 6 leaves","Vines",0.6154981467713354],
["classic","vine-straight-7","Upright vine · 7 leaves","Vines",0.40687984490847345],
["classic","vine-wave-7","Winding vine · 7 leaves","Vines",0.6232303927181244],
["classic","vine-straight-8","Upright vine · 8 leaves","Vines",0.40157837384356876],
["classic","vine-wave-8","Winding vine · 8 leaves","Vines",0.6182300625425566],
["classic","rosette-6","6-fold layered rosette","Flowers",1.125045525780579],
["classic","rosette-8","8-fold layered rosette","Flowers",1],
["classic","rosette-10","10-fold layered rosette","Flowers",1.0480166108348077],
["classic","rosette-12","12-fold layered rosette","Flowers",1],
["classic","leaf-3","Leaf · width 3","Leaves",15],
["classic","sprig-3","Leaf pair · width 3","Leaves",0.8782405537800217],
["classic","leaf-5","Leaf · width 5","Leaves",9],
["classic","sprig-5","Leaf pair · width 5","Leaves",0.8782405537800217],
["classic","leaf-8","Leaf · width 8","Leaves",5.625],
["classic","sprig-8","Leaf pair · width 8","Leaves",0.8782405537800217],
["classic","leaf-12","Leaf · width 12","Leaves",3.75],
["classic","sprig-12","Leaf pair · width 12","Leaves",0.8817254122281499],
["classic","star-4","4-point star","Symbols",1],
["classic","star-5","5-point star","Symbols",1.0514622242382674],
["classic","star-6","6-point star","Symbols",0.8660254037844387],
["classic","star-8","8-point star","Symbols",1],
["classic","star-10","10-point star","Symbols",0.9510565162951536],
["classic","star-12","12-point star","Symbols",1],
["classic","heart","Heart","Symbols",1.106642961622027],
["classic","double-heart","Nested hearts","Symbols",1.106642961622027],
["classic","arrow","Direction arrow","Symbols",1.125],
["classic","lightning","Lightning bolt","Symbols",0.8],
["classic","cross","Cross","Symbols",1],
["classic","badge-3","3-sided badge","Tags & badges",0.866025403784439],
["classic","badge-4","4-sided badge","Tags & badges",1],
["classic","badge-5","5-sided badge","Tags & badges",0.9510565162951535],
["classic","badge-6","6-sided badge","Tags & badges",1.1547005383792515],
["classic","badge-8","8-sided badge","Tags & badges",1],
["classic","round-tag","Round tag with hanging hole","Tags & badges",1],
["classic","rect-tag","Clipped gift tag","Tags & badges",0.8],
["classic","key-tag","Key tag","Tags & badges",2],
["classic","ring","Circular frame","Frames",1],
["classic","rect-frame","Rectangular frame","Frames",1],
["classic","diamond-frame","Diamond frame","Frames",1],
["classic","sunburst","Sunburst","Symbols",1],
["branches-0","collection-branch-0","Willow engraved branch","Botanical vines",4.767159873330743],
["branches-0","collection-vine-0","Willow climbing double vine","Vines",2.323349702830404],
["branches-0","collection-leaf-0","Willow veined leaf study","Leaves",2.6260504201680672],
["branches-0","collection-corner-0","Willow botanical corner","Botanical corners",1.0362654186937157],
["branches-0","collection-frame-0","Willow botanical invitation frame","Botanical frames",1.4243776118436495],
["branches-0","collection-wreath-0","Willow ceremonial wreath","Wreaths",1.0543986316127958],
["branches-1","collection-branch-1","Laurel engraved branch","Botanical vines",4.064941931625409],
["branches-1","collection-vine-1","Laurel climbing double vine","Vines",2.1528766899551344],
["branches-1","collection-leaf-1","Laurel veined leaf study","Leaves",2.7777777777777777],
["branches-1","collection-corner-1","Laurel botanical corner","Botanical corners",1.0308255183872577],
["branches-1","collection-frame-1","Laurel botanical invitation frame","Botanical frames",1.4192125127407702],
["branches-1","collection-wreath-1","Laurel ceremonial wreath","Wreaths",1.0508338083475284],
["branches-2","collection-branch-2","Eucalyptus engraved branch","Botanical vines",3.560207943383026],
["branches-2","collection-vine-2","Eucalyptus climbing double vine","Vines",2.0271838562308746],
["branches-2","collection-leaf-2","Eucalyptus veined leaf study","Leaves",2.5],
["branches-2","collection-corner-2","Eucalyptus botanical corner","Botanical corners",1.0290630657428064],
["branches-2","collection-frame-2","Eucalyptus botanical invitation frame","Botanical frames",1.4137281487471358],
["branches-2","collection-wreath-2","Eucalyptus ceremonial wreath","Wreaths",1.0497445956093578],
["branches-3","collection-branch-3","Rose engraved branch","Botanical vines",3.253899946438761],
["branches-3","collection-vine-3","Rose climbing double vine","Vines",1.961070609091733],
["branches-3","collection-leaf-3","Rose veined leaf study","Leaves",1.9337000798468629],
["branches-3","collection-corner-3","Rose botanical corner","Botanical corners",1.0313184005195113],
["branches-3","collection-frame-3","Rose botanical invitation frame","Botanical frames",1.4089405275931128],
["branches-3","collection-wreath-3","Rose ceremonial wreath","Wreaths",1.05551016618637],
["branches-4","collection-branch-4","Fern engraved branch","Botanical vines",4.39603468208812],
["branches-4","collection-vine-4","Fern climbing double vine","Vines",2.3195221196006943],
["branches-4","collection-leaf-4","Fern veined leaf study","Leaves",2.0833333333333335],
["branches-4","collection-corner-4","Fern botanical corner","Botanical corners",1.026817732050937],
["branches-4","collection-frame-4","Fern botanical invitation frame","Botanical frames",1.4215827417687776],
["branches-4","collection-wreath-4","Fern ceremonial wreath","Wreaths",1.0506398171478863],
["branches-5","collection-branch-5","Hazel engraved branch","Botanical vines",4.226181029494443],
["branches-5","collection-vine-5","Hazel climbing double vine","Vines",2.1869167429506104],
["branches-5","collection-leaf-5","Hazel veined leaf study","Leaves",3.125],
["branches-5","collection-corner-5","Hazel botanical corner","Botanical corners",1.0358222360232658],
["branches-5","collection-frame-5","Hazel botanical invitation frame","Botanical frames",1.4202726486238548],
["branches-5","collection-wreath-5","Hazel ceremonial wreath","Wreaths",1.0497476010505946],
["branches-6","collection-branch-6","Sage engraved branch","Botanical vines",3.5529793339759403],
["branches-6","collection-vine-6","Sage climbing double vine","Vines",1.9941428017414677],
["branches-6","collection-leaf-6","Sage veined leaf study","Leaves",2.437125769901121],
["branches-6","collection-corner-6","Sage botanical corner","Botanical corners",1.0304208233142853],
["branches-6","collection-frame-6","Sage botanical invitation frame","Botanical frames",1.4139964045879203],
["branches-6","collection-wreath-6","Sage ceremonial wreath","Wreaths",1.0500530921922242],
["branches-7","collection-branch-7","Myrtle engraved branch","Botanical vines",3.1800242639813865],
["branches-7","collection-vine-7","Myrtle climbing double vine","Vines",1.8835393110313858],
["branches-7","collection-leaf-7","Myrtle veined leaf study","Leaves",2.5],
["branches-7","collection-corner-7","Myrtle botanical corner","Botanical corners",1.0288225166002292],
["branches-7","collection-frame-7","Myrtle botanical invitation frame","Botanical frames",1.4090564369290368],
["branches-7","collection-wreath-7","Myrtle ceremonial wreath","Wreaths",1.0540384409785997],
["branches-8","collection-branch-8","Ash engraved branch","Botanical vines",4.620001423577838],
["branches-8","collection-vine-8","Ash climbing double vine","Vines",2.377240212010374],
["branches-8","collection-leaf-8","Ash veined leaf study","Leaves",2.272727272727273],
["branches-8","collection-corner-8","Ash botanical corner","Botanical corners",1.0325742847685573],
["branches-8","collection-frame-8","Ash botanical invitation frame","Botanical frames",1.4227502603641173],
["branches-8","collection-wreath-8","Ash ceremonial wreath","Wreaths",1.0501096769144902],
["branches-9","collection-branch-9","Camellia engraved branch","Botanical vines",3.846408269026012],
["branches-9","collection-vine-9","Camellia climbing double vine","Vines",2.1570627000744436],
["branches-9","collection-leaf-9","Camellia veined leaf study","Leaves",1.8335194492117768],
["branches-9","collection-corner-9","Camellia botanical corner","Botanical corners",1.0264762920017847],
["branches-9","collection-frame-9","Camellia botanical invitation frame","Botanical frames",1.4165667065558392],
["branches-9","collection-wreath-9","Camellia ceremonial wreath","Wreaths",1.0477770628186427],
["branches-10","collection-branch-10","Bay engraved branch","Botanical vines",3.687232310856525],
["branches-10","collection-vine-10","Bay climbing double vine","Vines",2.03313745773419],
["branches-10","collection-leaf-10","Bay veined leaf study","Leaves",3.1239833355163933],
["branches-10","collection-corner-10","Bay botanical corner","Botanical corners",1.0352671166214733],
["branches-10","collection-frame-10","Bay botanical invitation frame","Botanical frames",1.4151009064515012],
["branches-10","collection-wreath-10","Bay ceremonial wreath","Wreaths",1.0488488649979173],
["branches-11","collection-branch-11","Birch engraved branch","Botanical vines",3.1709618175204985],
["branches-11","collection-vine-11","Birch climbing double vine","Vines",1.8487114931411166],
["branches-11","collection-leaf-11","Birch veined leaf study","Leaves",2.775451883793978],
["branches-11","collection-corner-11","Birch botanical corner","Botanical corners",1.0302022740449026],
["branches-11","collection-frame-11","Birch botanical invitation frame","Botanical frames",1.4093845671731957],
["branches-11","collection-wreath-11","Birch ceremonial wreath","Wreaths",1.0487905727985756],
["blossoms-0","collection-bloom-0","Dahlia layered bloom","Flowers",0.9590893304465131],
["blossoms-0","collection-stem-0","Dahlia botanical stem","Flowers",0.5772768340137949],
["blossoms-0","collection-bouquet-0","Dahlia three-flower bouquet","Flowers",0.8212564087712005],
["blossoms-1","collection-bloom-1","Cosmos layered bloom","Flowers",1.1437945639784033],
["blossoms-1","collection-stem-1","Cosmos botanical stem","Flowers",0.6485956669891912],
["blossoms-1","collection-bouquet-1","Cosmos three-flower bouquet","Flowers",0.8446958698236948],
["blossoms-2","collection-bloom-2","Daisy layered bloom","Flowers",0.9771966829147891],
["blossoms-2","collection-stem-2","Daisy botanical stem","Flowers",0.5983840681912159],
["blossoms-2","collection-bouquet-2","Daisy three-flower bouquet","Flowers",0.8268798429097026],
["blossoms-3","collection-bloom-3","Aster layered bloom","Flowers",1],
["blossoms-3","collection-stem-3","Aster botanical stem","Flowers",0.6231884057971014],
["blossoms-3","collection-bouquet-3","Aster three-flower bouquet","Flowers",0.8197434782789182],
["blossoms-4","collection-bloom-4","Zinnia layered bloom","Flowers",0.9854150501437412],
["blossoms-4","collection-stem-4","Zinnia botanical stem","Flowers",0.6076461721342841],
["blossoms-4","collection-bouquet-4","Zinnia three-flower bouquet","Flowers",0.8279049982993494],
["blossoms-5","collection-bloom-5","Gazania layered bloom","Flowers",1.0511564469197485],
["blossoms-5","collection-stem-5","Gazania botanical stem","Flowers",0.632784134606528],
["blossoms-5","collection-bouquet-5","Gazania three-flower bouquet","Flowers",0.8292259877417509],
["blossoms-6","collection-bloom-6","Chrysanthemum layered bloom","Flowers",0.9898214418809328],
["blossoms-6","collection-stem-6","Chrysanthemum botanical stem","Flowers",0.6125092731779006],
["blossoms-6","collection-bouquet-6","Chrysanthemum three-flower bouquet","Flowers",0.8293953323676666],
["blossoms-7","collection-bloom-7","Gerbera layered bloom","Flowers",1],
["blossoms-7","collection-stem-7","Gerbera botanical stem","Flowers",0.6231884057971014],
["blossoms-7","collection-bouquet-7","Gerbera three-flower bouquet","Flowers",0.8236740671126237],
["blossoms-8","collection-bloom-8","Anemone layered bloom","Flowers",0.992708874098054],
["blossoms-8","collection-stem-8","Anemone botanical stem","Flowers",0.6155324558587931],
["blossoms-8","collection-bouquet-8","Anemone three-flower bouquet","Flowers",0.8328929813142829],
["blossoms-9","collection-bloom-9","Marigold layered bloom","Flowers",1.0257168632725537],
["blossoms-9","collection-stem-9","Marigold botanical stem","Flowers",0.628095285410301],
["blossoms-9","collection-bouquet-9","Marigold three-flower bouquet","Flowers",0.8336025580327482],
["blossoms-10","collection-bloom-10","Coreopsis layered bloom","Flowers",0.9945218953682733],
["blossoms-10","collection-stem-10","Coreopsis botanical stem","Flowers",0.6174332498639544],
["blossoms-10","collection-bouquet-10","Coreopsis three-flower bouquet","Flowers",0.8343116932370306],
["blossoms-11","collection-bloom-11","Calendula layered bloom","Flowers",1],
["blossoms-11","collection-stem-11","Calendula botanical stem","Flowers",0.6231884057971014],
["blossoms-11","collection-bouquet-11","Calendula three-flower bouquet","Flowers",0.8319300094703853],
["flourishes-0","collection-scroll-0","Regency scroll 1","Ornamental swirls",3.0259714402722926],
["flourishes-0","collection-cartouche-0","Oval ornamental frame 1","Frames",1.2078604444257866],
["flourishes-1","collection-scroll-1","Rococo scroll 1","Ornamental swirls",2.6751584479826866],
["flourishes-1","collection-cartouche-1","Cameo ornamental frame 1","Frames",1.1536138736611092],
["flourishes-2","collection-scroll-2","Palmette scroll 1","Ornamental swirls",2.796608223462163],
["flourishes-2","collection-cartouche-2","Medallion ornamental frame 1","Frames",1.1733534869391526],
["flourishes-3","collection-scroll-3","Baroque scroll 1","Ornamental swirls",2.6751584479826866],
["flourishes-3","collection-cartouche-3","Crest ornamental frame 1","Frames",1.1536138736611092],
["flourishes-4","collection-scroll-4","Regency scroll 2","Ornamental swirls",2.952832138684597],
["flourishes-4","collection-cartouche-4","Oval ornamental frame 2","Frames",1.1972269940139684],
["flourishes-5","collection-scroll-5","Rococo scroll 2","Ornamental swirls",2.6751584479826866],
["flourishes-5","collection-cartouche-5","Cameo ornamental frame 2","Frames",1.1536138736611092],
["flourishes-6","collection-scroll-6","Palmette scroll 2","Ornamental swirls",2.9646155040759536],
["flourishes-6","collection-cartouche-6","Medallion ornamental frame 2","Frames",1.1989627077944813],
["flourishes-7","collection-scroll-7","Baroque scroll 2","Ornamental swirls",2.6751584479826866],
["flourishes-7","collection-cartouche-7","Crest ornamental frame 2","Frames",1.1536138736611092],
["flourishes-8","collection-scroll-8","Regency scroll 3","Ornamental swirls",2.320748655520056],
["flourishes-8","collection-cartouche-8","Oval ornamental frame 3","Frames",1.174343659744694],
["flourishes-9","collection-scroll-9","Rococo scroll 3","Ornamental swirls",2.2324966974900926],
["flourishes-9","collection-cartouche-9","Cameo ornamental frame 3","Frames",1.1536138736611092],
["flourishes-10","collection-scroll-10","Palmette scroll 3","Ornamental swirls",2.4299863931861836],
["flourishes-10","collection-cartouche-10","Medallion ornamental frame 3","Frames",1.1988417102443056],
["flourishes-11","collection-scroll-11","Baroque scroll 3","Ornamental swirls",2.2324966974900926],
["flourishes-11","collection-cartouche-11","Crest ornamental frame 3","Frames",1.1536138736611092],
["flourishes-12","collection-scroll-12","Regency scroll 4","Ornamental swirls",2.471627290653299],
["flourishes-12","collection-cartouche-12","Oval ornamental frame 4","Frames",1.2078604444257866],
["flourishes-13","collection-scroll-13","Rococo scroll 4","Ornamental swirls",2.2324966974900926],
["flourishes-13","collection-cartouche-13","Cameo ornamental frame 4","Frames",1.1536138736611092],
["flourishes-14","collection-scroll-14","Palmette scroll 4","Ornamental swirls",2.3233402048353335],
["flourishes-14","collection-cartouche-14","Medallion ornamental frame 4","Frames",1.1749394082560132],
["flourishes-15","collection-scroll-15","Baroque scroll 4","Ornamental swirls",2.2324966974900926],
["flourishes-15","collection-cartouche-15","Crest ornamental frame 4","Frames",1.1536138736611092],
["borders-0","collection-border-0","Scroll repeating border 1","Decorative borders",2.9411764705882355],
["borders-1","collection-border-1","Scroll repeating border 2","Decorative borders",3.8970588235294117],
["borders-2","collection-border-2","Scroll repeating border 3","Decorative borders",4.852941176470588],
["borders-3","collection-border-3","Scroll repeating border 4","Decorative borders",5.8088235294117645],
["borders-4","collection-border-4","Scroll repeating border 5","Decorative borders",2.9411764705882355],
["borders-5","collection-border-5","Scroll repeating border 6","Decorative borders",3.8970588235294117],
["borders-6","collection-border-6","Scroll repeating border 7","Decorative borders",4.852941176470588],
["borders-7","collection-border-7","Scroll repeating border 8","Decorative borders",5.8088235294117645],
["borders-8","collection-border-8","Botanical repeating border 1","Decorative borders",3.161764705882353],
["borders-9","collection-border-9","Botanical repeating border 2","Decorative borders",4.126015850768685],
["borders-10","collection-border-10","Botanical repeating border 3","Decorative borders",5.08986994311822],
["borders-11","collection-border-11","Botanical repeating border 4","Decorative borders",6.053007394334861],
["borders-12","collection-border-12","Botanical repeating border 5","Decorative borders",3.194497591642367],
["borders-13","collection-border-13","Botanical repeating border 6","Decorative borders",4.117647058823529],
["borders-14","collection-border-14","Botanical repeating border 7","Decorative borders",5.0797410581400815],
["borders-15","collection-border-15","Botanical repeating border 8","Decorative borders",6.04046957959936],
["borders-16","collection-border-16","Floral repeating border 1","Decorative borders",2.9411764705882355],
["borders-17","collection-border-17","Floral repeating border 2","Decorative borders",3.8970588235294117],
["borders-18","collection-border-18","Floral repeating border 3","Decorative borders",4.852941176470588],
["borders-19","collection-border-19","Floral repeating border 4","Decorative borders",5.8088235294117645],
["borders-20","collection-border-20","Floral repeating border 5","Decorative borders",2.9411764705882355],
["borders-21","collection-border-21","Floral repeating border 6","Decorative borders",3.8970588235294117],
["borders-22","collection-border-22","Floral repeating border 7","Decorative borders",4.852941176470588],
["borders-23","collection-border-23","Floral repeating border 8","Decorative borders",5.8088235294117645],
["mandalas-0","collection-mandala-0","6-fold lotus mandala","Mandalas",1],
["mandalas-1","collection-mandala-1","7-fold lotus mandala","Mandalas",1],
["mandalas-2","collection-mandala-2","8-fold lotus mandala","Mandalas",1],
["mandalas-3","collection-mandala-3","9-fold lotus mandala","Mandalas",1],
["mandalas-4","collection-mandala-4","10-fold lotus mandala","Mandalas",1],
["mandalas-5","collection-mandala-5","11-fold lotus mandala","Mandalas",1],
["mandalas-6","collection-mandala-6","12-fold lotus mandala","Mandalas",1],
["mandalas-7","collection-mandala-7","13-fold lotus mandala","Mandalas",1],
["mandalas-8","collection-mandala-8","14-fold lotus mandala","Mandalas",1],
["mandalas-9","collection-mandala-9","15-fold lotus mandala","Mandalas",1],
["mandalas-10","collection-mandala-10","16-fold lotus mandala","Mandalas",1],
["mandalas-11","collection-mandala-11","17-fold lotus mandala","Mandalas",1],
["mandalas-12","collection-mandala-12","18-fold lotus mandala","Mandalas",1],
["mandalas-13","collection-mandala-13","19-fold lotus mandala","Mandalas",1],
["mandalas-14","collection-mandala-14","20-fold lotus mandala","Mandalas",1],
["mandalas-15","collection-mandala-15","21-fold lotus mandala","Mandalas",1],
["seasonal-0","collection-snow-0","Crystal snowflake 1","Seasonal",0.8660254037844388],
["seasonal-1","collection-snow-1","Crystal snowflake 2","Seasonal",0.8660254037844388],
["seasonal-2","collection-snow-2","Crystal snowflake 3","Seasonal",0.9024327626173179],
["seasonal-3","collection-snow-3","Crystal snowflake 4","Seasonal",0.8660254037844388],
["seasonal-4","collection-snow-4","Crystal snowflake 5","Seasonal",0.8660254037844388],
["seasonal-5","collection-snow-5","Crystal snowflake 6","Seasonal",0.8910691262536815],
["seasonal-6","collection-snow-6","Crystal snowflake 7","Seasonal",0.8728624280761725],
["seasonal-7","collection-snow-7","Crystal snowflake 8","Seasonal",0.870139946594989],
["seasonal-8","collection-snow-8","Crystal snowflake 9","Seasonal",0.8797054898900452],
["seasonal-9","collection-snow-9","Crystal snowflake 10","Seasonal",0.8685404471313493],
["seasonal-10","collection-snow-10","Crystal snowflake 11","Seasonal",0.8690867108833245],
["seasonal-11","collection-snow-11","Crystal snowflake 12","Seasonal",0.9137963989809542],
["labels-0","collection-label-0","Gift engraved label 1","Tags & badges",1.8571428571428572],
["labels-1","collection-label-1","Gift engraved label 2","Tags & badges",2.085714285714286],
["labels-2","collection-label-2","Gift engraved label 3","Tags & badges",2.3142857142857145],
["labels-3","collection-label-3","Gift engraved label 4","Tags & badges",2.5428571428571427],
["labels-4","collection-label-4","Pantry engraved label 1","Tags & badges",1.625],
["labels-5","collection-label-5","Pantry engraved label 2","Tags & badges",1.825],
["labels-6","collection-label-6","Pantry engraved label 3","Tags & badges",2.025],
["labels-7","collection-label-7","Pantry engraved label 4","Tags & badges",2.225],
["labels-8","collection-label-8","Luggage engraved label 1","Tags & badges",1.4444444444444444],
["labels-9","collection-label-9","Luggage engraved label 2","Tags & badges",1.6222222222222222],
["labels-10","collection-label-10","Luggage engraved label 3","Tags & badges",1.8],
["labels-11","collection-label-11","Luggage engraved label 4","Tags & badges",1.9777777777777779],
["labels-12","collection-label-12","Garden engraved label 1","Tags & badges",1.3],
["labels-13","collection-label-13","Garden engraved label 2","Tags & badges",1.46],
["labels-14","collection-label-14","Garden engraved label 3","Tags & badges",1.62],
["labels-15","collection-label-15","Garden engraved label 4","Tags & badges",1.78],
["butterflies-0","collection-butterfly-0","Swallowtail butterfly study 1","Butterflies",1.188886947405207],
["butterflies-1","collection-butterfly-1","Meadow butterfly study 1","Butterflies",1.2264307457443189],
["butterflies-2","collection-butterfly-2","Lacewing butterfly study 1","Butterflies",1.2639745440834307],
["butterflies-3","collection-butterfly-3","Garden butterfly study 1","Butterflies",1.3026734230298795],
["butterflies-4","collection-butterfly-4","Swallowtail butterfly study 2","Butterflies",1.188886947405207],
["butterflies-5","collection-butterfly-5","Meadow butterfly study 2","Butterflies",1.2264307457443189],
["butterflies-6","collection-butterfly-6","Lacewing butterfly study 2","Butterflies",1.2639745440834307],
["butterflies-7","collection-butterfly-7","Garden butterfly study 2","Butterflies",1.3026734230298795],
["butterflies-8","collection-butterfly-8","Swallowtail butterfly study 3","Butterflies",1.188886947405207],
["butterflies-9","collection-butterfly-9","Meadow butterfly study 3","Butterflies",1.2264307457443189],
["butterflies-10","collection-butterfly-10","Lacewing butterfly study 3","Butterflies",1.2639745440834307],
["butterflies-11","collection-butterfly-11","Garden butterfly study 3","Butterflies",1.3026734230298795],
["landscapes-0","collection-landscape-0","Alpine lake landscape badge 1","Nature & landscapes",1],
["landscapes-1","collection-landscape-1","Mountain river landscape badge 1","Nature & landscapes",1],
["landscapes-2","collection-landscape-2","Forest ridge landscape badge 1","Nature & landscapes",1],
["landscapes-3","collection-landscape-3","Alpine lake landscape badge 2","Nature & landscapes",1],
["landscapes-4","collection-landscape-4","Mountain river landscape badge 2","Nature & landscapes",1],
["landscapes-5","collection-landscape-5","Forest ridge landscape badge 2","Nature & landscapes",1],
["landscapes-6","collection-landscape-6","Alpine lake landscape badge 3","Nature & landscapes",1],
["landscapes-7","collection-landscape-7","Mountain river landscape badge 3","Nature & landscapes",1],
["landscapes-8","collection-landscape-8","Forest ridge landscape badge 3","Nature & landscapes",1],
["landscapes-9","collection-landscape-9","Alpine lake landscape badge 4","Nature & landscapes",1],
["landscapes-10","collection-landscape-10","Mountain river landscape badge 4","Nature & landscapes",1],
["landscapes-11","collection-landscape-11","Forest ridge landscape badge 4","Nature & landscapes",1],
["patterns-0","collection-pattern-0","Diamond lattice panel 1","Geometric patterns",1],
["patterns-1","collection-pattern-1","Diamond lattice panel 2","Geometric patterns",1],
["patterns-2","collection-pattern-2","Diamond lattice panel 3","Geometric patterns",1],
["patterns-3","collection-pattern-3","Diamond lattice panel 4","Geometric patterns",1],
["patterns-4","collection-pattern-4","Polygon tessellation panel 1","Geometric patterns",1],
["patterns-5","collection-pattern-5","Polygon tessellation panel 2","Geometric patterns",0.9991444047036673],
["patterns-6","collection-pattern-6","Polygon tessellation panel 3","Geometric patterns",1],
["patterns-7","collection-pattern-7","Polygon tessellation panel 4","Geometric patterns",0.9999362663270897],
["patterns-8","collection-pattern-8","Scallop repeat panel 1","Geometric patterns",0.9787234042553191],
["patterns-9","collection-pattern-9","Scallop repeat panel 2","Geometric patterns",0.968421052631579],
["patterns-10","collection-pattern-10","Scallop repeat panel 3","Geometric patterns",0.9583333333333334],
["patterns-11","collection-pattern-11","Scallop repeat panel 4","Geometric patterns",0.9484536082474226],
["patterns-12","collection-pattern-12","Leaf parquet panel 1","Geometric patterns",0.9999999999999999],
["patterns-13","collection-pattern-13","Leaf parquet panel 2","Geometric patterns",1],
["patterns-14","collection-pattern-14","Leaf parquet panel 3","Geometric patterns",1],
["patterns-15","collection-pattern-15","Leaf parquet panel 4","Geometric patterns",1],
["symbols-0","collection-compass-0","5-ray engraved compass medallion","Symbols",1.0514622242382674],
["symbols-1","collection-compass-1","6-ray engraved compass medallion","Symbols",0.8660254037844387],
["symbols-2","collection-compass-2","7-ray engraved compass medallion","Symbols",1.025716863272554],
["symbols-3","collection-compass-3","8-ray engraved compass medallion","Symbols",1],
["symbols-4","collection-compass-4","9-ray engraved compass medallion","Symbols",1.015426611885745],
["symbols-5","collection-compass-5","10-ray engraved compass medallion","Symbols",0.9510565162951536],
["symbols-6","collection-compass-6","11-ray engraved compass medallion","Symbols",1.0102832265380361],
["symbols-7","collection-compass-7","12-ray engraved compass medallion","Symbols",1],
["symbols-8","collection-compass-8","13-ray engraved compass medallion","Symbols",1.0073446768656829],
["symbols-9","collection-compass-9","14-ray engraved compass medallion","Symbols",0.9749279121818237],
["symbols-10","collection-compass-10","15-ray engraved compass medallion","Symbols",1.0055082795635162],
["symbols-11","collection-compass-11","16-ray engraved compass medallion","Symbols",1],
["symbols-12","collection-compass-12","17-ray engraved compass medallion","Symbols",1.004284098915674],
["symbols-13","collection-compass-13","18-ray engraved compass medallion","Symbols",0.9848077530122079],
["symbols-14","collection-compass-14","19-ray engraved compass medallion","Symbols",1.0034272126621453],
["symbols-15","collection-compass-15","20-ray engraved compass medallion","Symbols",1],
["daily-life","purpose-nautical-coastal-sailboat","Coastal sailboat","Nautical",0.9836065573770492],
["daily-life","purpose-nautical-admiralty-anchor","Admiralty anchor","Nautical",0.9649805447470817],
["daily-life","purpose-nautical-ship-wheel","Ship wheel","Nautical",1],
["daily-life","purpose-nautical-scallop-seashell","Scallop seashell","Nautical",1.4545454545454546],
["daily-life","purpose-nautical-lighthouse-beacon","Lighthouse beacon","Nautical",1.095890410958904],
["daily-life","purpose-nautical-rolling-ocean-waves","Rolling ocean waves","Nautical",2.1140099610332634],
["daily-life","purpose-nautical-nautilus-shell","Nautilus shell","Nautical",0.940774578125512],
["daily-life","purpose-nautical-mariner-compass","Mariner compass","Nautical",1],
["daily-life","purpose-space-astronomy-ringed-planet","Ringed planet","Space & astronomy",1.7391304347826086],
["daily-life","purpose-space-astronomy-retro-rocket","Retro rocket","Space & astronomy",0.6133333333333333],
["daily-life","purpose-space-astronomy-crescent-moon","Crescent moon","Space & astronomy",0.7500839330342496],
["daily-life","purpose-space-astronomy-solar-system-orbits","Solar system orbits","Space & astronomy",0.963525703942503],
["daily-life","purpose-space-astronomy-shooting-star","Shooting star","Space & astronomy",1.3746374807257382],
["daily-life","purpose-space-astronomy-lunar-phases","Lunar phases","Space & astronomy",6.333333333333333],
["daily-life","purpose-space-astronomy-constellation-study","Constellation study","Space & astronomy",1.65],
["daily-life","purpose-space-astronomy-observatory-telescope","Observatory telescope","Space & astronomy",0.9142857142857143],
["daily-life","purpose-music-quaver-note","Quaver note","Music",0.7317073170731708],
["daily-life","purpose-music-beamed-eighth-notes","Beamed eighth notes","Music",0.7784431137724552],
["daily-life","purpose-music-acoustic-guitar","Acoustic guitar","Music",0.53870159404144],
["daily-life","purpose-music-piano-keyboard","Piano keyboard","Music",1.75],
["daily-life","purpose-music-studio-headphones","Studio headphones","Music",1.2444444444444445],
["daily-life","purpose-music-vinyl-record","Vinyl record","Music",1],
["daily-life","purpose-music-concert-microphone","Concert microphone","Music",0.5925925925925926],
["daily-life","purpose-music-audio-waveform","Audio waveform","Music",1.8539016683847427],
["daily-life","purpose-workshop-makers-crossed-spanners","Crossed spanners","Workshop & makers",0.8588596831417832],
["daily-life","purpose-workshop-makers-claw-hammer","Claw hammer","Workshop & makers",0.6964285714285714],
["daily-life","purpose-workshop-makers-carpenters-square","Carpenters square","Workshop & makers",1],
["daily-life","purpose-workshop-makers-hand-saw","Hand saw","Workshop & makers",3.3],
["daily-life","purpose-workshop-makers-hex-nut","Hex nut","Workshop & makers",1.1547005383792515],
["daily-life","purpose-workshop-makers-bench-clamp","Bench clamp","Workshop & makers",0.7166666666666667],
["daily-life","purpose-workshop-makers-digital-caliper","Digital caliper","Workshop & makers",2.5],
["daily-life","purpose-workshop-makers-sewing-needle-and-thread","Sewing needle and thread","Workshop & makers",0.46875],
["daily-life","purpose-food-kitchen-coffee-cup","Coffee cup","Food & kitchen",0.9900414251512096],
["daily-life","purpose-food-kitchen-teapot","Teapot","Food & kitchen",2.0304878048780486],
["daily-life","purpose-food-kitchen-wine-glass","Wine glass","Food & kitchen",0.625],
["daily-life","purpose-food-kitchen-artisan-bread","Artisan bread","Food & kitchen",1.9936631944444445],
["daily-life","purpose-food-kitchen-chef-hat","Chef hat","Food & kitchen",1.1469106122481922],
["daily-life","purpose-food-kitchen-honeycomb","Honeycomb","Food & kitchen",1.1547005383792515],
["daily-life","purpose-food-kitchen-apple-harvest","Apple harvest","Food & kitchen",0.8416346122234277],
["daily-life","purpose-food-kitchen-cutlery-place-setting","Cutlery place setting","Food & kitchen",0.6557377049180327],
["daily-life","purpose-travel-outdoors-camping-tent","Camping tent","Travel & outdoors",1.5714285714285714],
["daily-life","purpose-travel-outdoors-mountain-hiking-boot","Mountain hiking boot","Travel & outdoors",1.3191489361702127],
["daily-life","purpose-travel-outdoors-trail-backpack","Trail backpack","Travel & outdoors",0.9032258064516129],
["daily-life","purpose-travel-outdoors-navigation-pin","Navigation pin","Travel & outdoors",0.6451612903225806],
["daily-life","purpose-travel-outdoors-caravan","Caravan","Travel & outdoors",1.4727272727272727],
["daily-life","purpose-travel-outdoors-trail-binoculars","Trail binoculars","Travel & outdoors",1.25],
["daily-life","purpose-travel-outdoors-campfire","Campfire","Travel & outdoors",0.8098248016838349],
["daily-life","purpose-travel-outdoors-travel-suitcase","Travel suitcase","Travel & outdoors",1.2075471698113207],
["daily-life","purpose-garden-growing-watering-can","Watering can","Garden & growing",2.0457317073170733],
["daily-life","purpose-garden-growing-potted-seedling","Potted seedling","Garden & growing",0.6002643007559472],
["daily-life","purpose-garden-growing-garden-trowel","Garden trowel","Garden & growing",0.4],
["daily-life","purpose-garden-growing-mushroom-pair","Mushroom pair","Garden & growing",1.018181818181818],
["daily-life","purpose-garden-growing-dragonfly","Dragonfly","Garden & growing",1.128608838229843],
["daily-life","purpose-garden-growing-honey-bee","Honey bee","Garden & growing",1.307077387698911],
["daily-life","purpose-garden-growing-butterfly-garden-gate","Butterfly garden gate","Garden & growing",1.0666666666666667],
["daily-life","purpose-garden-growing-sprouting-acorn","Sprouting acorn","Garden & growing",0.8780487804878049],
["daily-life","purpose-celebrations-wedding-ring-pair","Wedding ring pair","Celebrations",1.3076923076923077],
["daily-life","purpose-celebrations-birthday-cake","Birthday cake","Celebrations",1.1],
["daily-life","purpose-celebrations-wrapped-gift","Wrapped gift","Celebrations",0.9050247212066079],
["daily-life","purpose-celebrations-party-balloon-trio","Party balloon trio","Celebrations",0.6600660066006601],
["daily-life","purpose-celebrations-royal-crown","Royal crown","Celebrations",1.4782608695652173],
["daily-life","purpose-celebrations-love-letter","Love letter","Celebrations",1.4782608695652173],
["daily-life","purpose-celebrations-graduation-cap","Graduation cap","Celebrations",1.4615384615384615],
["daily-life","purpose-celebrations-anniversary-ribbon","Anniversary ribbon","Celebrations",3.52],
["daily-life","purpose-animals-pets-paw-print","Paw print","Animals & pets",1.16],
["daily-life","purpose-animals-pets-cat-portrait","Cat portrait","Animals & pets",1.3333333333333333],
["daily-life","purpose-animals-pets-dog-bone","Dog bone","Animals & pets",2.1333333333333333],
["daily-life","purpose-animals-pets-songbird","Songbird","Animals & pets",1.2910917808143425],
["daily-life","purpose-animals-pets-koi-fish","Koi fish","Animals & pets",1.8942070972919303],
["daily-life","purpose-animals-pets-wise-owl","Wise owl","Animals & pets",0.9803921568627451],
["daily-life","purpose-animals-pets-sea-turtle","Sea turtle","Animals & pets",0.9473684210526315],
["daily-life","purpose-animals-pets-rabbit-silhouette","Rabbit silhouette","Animals & pets",0.6260160135116603],
["daily-life","purpose-coaster-0","Coastal sailboat coaster","Coasters",1],
["daily-life","purpose-bookmark-0","Coastal sailboat bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-0","Coastal sailboat hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-1","Scallop seashell coaster","Coasters",1],
["daily-life","purpose-bookmark-1","Scallop seashell bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-1","Scallop seashell hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-2","Nautilus shell coaster","Coasters",1],
["daily-life","purpose-bookmark-2","Nautilus shell bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-2","Nautilus shell hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-3","Retro rocket coaster","Coasters",1],
["daily-life","purpose-bookmark-3","Retro rocket bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-3","Retro rocket hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-4","Shooting star coaster","Coasters",1],
["daily-life","purpose-bookmark-4","Shooting star bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-4","Shooting star hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-5","Observatory telescope coaster","Coasters",1],
["daily-life","purpose-bookmark-5","Observatory telescope bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-5","Observatory telescope hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-6","Acoustic guitar coaster","Coasters",1],
["daily-life","purpose-bookmark-6","Acoustic guitar bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-6","Acoustic guitar hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-7","Vinyl record coaster","Coasters",1],
["daily-life","purpose-bookmark-7","Vinyl record bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-7","Vinyl record hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-8","Crossed spanners coaster","Coasters",1],
["daily-life","purpose-bookmark-8","Crossed spanners bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-8","Crossed spanners hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-9","Hand saw coaster","Coasters",1],
["daily-life","purpose-bookmark-9","Hand saw bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-9","Hand saw hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-10","Digital caliper coaster","Coasters",1],
["daily-life","purpose-bookmark-10","Digital caliper bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-10","Digital caliper hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-11","Teapot coaster","Coasters",1],
["daily-life","purpose-bookmark-11","Teapot bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-11","Teapot hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-12","Chef hat coaster","Coasters",1],
["daily-life","purpose-bookmark-12","Chef hat bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-12","Chef hat hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-13","Cutlery place setting coaster","Coasters",1],
["daily-life","purpose-bookmark-13","Cutlery place setting bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-13","Cutlery place setting hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-14","Trail backpack coaster","Coasters",1],
["daily-life","purpose-bookmark-14","Trail backpack bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-14","Trail backpack hanging ornament","Hanging ornaments",1],
["daily-life","purpose-coaster-15","Trail binoculars coaster","Coasters",1],
["daily-life","purpose-bookmark-15","Trail binoculars bookmark","Bookmarks",0.26153846153846155],
["daily-life","purpose-ornament-15","Trail binoculars hanging ornament","Hanging ornaments",1],
["daily-life","purpose-earrings-0","Round botanical earring pair 1","Jewellery",2.25],
["daily-life","purpose-earrings-1","Diamond botanical earring pair 1","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-2","Teardrop botanical earring pair 1","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-3","Round botanical earring pair 2","Jewellery",2.25],
["daily-life","purpose-earrings-4","Diamond botanical earring pair 2","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-5","Teardrop botanical earring pair 2","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-6","Round botanical earring pair 3","Jewellery",2.25],
["daily-life","purpose-earrings-7","Diamond botanical earring pair 3","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-8","Teardrop botanical earring pair 3","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-9","Round botanical earring pair 4","Jewellery",2.25],
["daily-life","purpose-earrings-10","Diamond botanical earring pair 4","Jewellery",1.7916666666666667],
["daily-life","purpose-earrings-11","Teardrop botanical earring pair 4","Jewellery",1.7916666666666667],
["landmarks","expansion-opera-house","Sydney Opera House · sail roofs","World landmarks",2.2131147540983607],
["landmarks","expansion-harbour-bridge","Sydney Harbour Bridge · steel arch","World landmarks",2.4193548387096775],
["landmarks","expansion-eiffel-tower","Eiffel Tower · lattice elevation","World landmarks",0.6306306306306306],
["landmarks","expansion-statue-liberty","Statue of Liberty · torch and crown","World landmarks",0.44954128440366975],
["landmarks","expansion-taj-mahal","Taj Mahal · dome and minarets","World landmarks",1.411764705882353],
["landmarks","expansion-elizabeth-tower","Elizabeth Tower · London clock tower","World landmarks",0.3076923076923077],
["landmarks","expansion-pisa","Leaning Tower of Pisa · arcades","World landmarks",0.5131313131313131],
["landmarks","expansion-colosseum","Roman Colosseum · tiered arches","World landmarks",2.04],
["landmarks","expansion-arc-triomphe","Arc de Triomphe · monumental arch","World landmarks",1.2280701754385965],
["landmarks","expansion-giza","Pyramids of Giza · desert skyline","World landmarks",2.611940298507463],
["landmarks","expansion-five-storey-pagoda","Japanese five-storey pagoda","World landmarks",1.0217391304347827],
["landmarks","expansion-golden-gate","Golden Gate Bridge · suspension span","World landmarks",2.1311475409836067],
["architecture","expansion-heritage-0","English thatched cottage","Historic buildings",1.4035087719298245],
["architecture","expansion-heritage-1","French village bakery","Historic buildings",1.4426229508196722],
["architecture","expansion-heritage-2","Victorian terrace house","Historic buildings",1.2151898734177216],
["architecture","expansion-heritage-3","Old stone chapel","Historic buildings",1.4736842105263157],
["architecture","expansion-heritage-4","Dutch canal house","Historic buildings",1.1594202898550725],
["architecture","expansion-heritage-5","Alpine timber chalet","Historic buildings",1.0864197530864197],
["architecture","expansion-heritage-6","Tuscan farmhouse","Historic buildings",1.9591836734693877],
["architecture","expansion-heritage-7","Georgian town house","Historic buildings",1.5522388059701493],
["architecture","expansion-heritage-8","Art Deco cinema","Historic buildings",0.9876543209876543],
["architecture","expansion-heritage-9","Timber railway station","Historic buildings",1.9130434782608696],
["architecture","expansion-heritage-10","Colonial veranda house","Historic buildings",1.391304347826087],
["architecture","expansion-heritage-11","Medieval gatehouse","Historic buildings",1.12],
["architecture","expansion-heritage-12","Country watermill","Historic buildings",1.6551724137931034],
["architecture","expansion-heritage-13","Historic fire station","Historic buildings",1.5172413793103448],
["architecture","expansion-heritage-14","Old coastal warehouse","Historic buildings",1.6],
["architecture","expansion-heritage-15","Half-timbered inn","Historic buildings",1.8245614035087718],
["architecture","expansion-heritage-16","Prairie schoolhouse","Historic buildings",1.1594202898550725],
["architecture","expansion-heritage-17","Stone blacksmith forge","Historic buildings",1.1139240506329113],
["architecture","expansion-heritage-18","Traditional windmill","Historic buildings",1.1038793007306982],
["architecture","expansion-heritage-19","Vintage general store","Historic buildings",2.1666666666666665],
["scenery","expansion-mountain-0","Alpine lake · scene 1","Mountain scenes",1.6049382716049383],
["scenery","expansion-river-0","Meandering river · scene 1","River scenes",1.3131313131313131],
["scenery","expansion-mountain-1","Pine ridge · scene 1","Mountain scenes",1.5853658536585367],
["scenery","expansion-river-1","Riverside sailing · scene 1","River scenes",1.3],
["scenery","expansion-mountain-2","Summit campsite · scene 1","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-2","Woodland river camp · scene 1","River scenes",1.2871287128712872],
["scenery","expansion-mountain-3","Highland panorama · scene 1","Mountain scenes",1.5476190476190477],
["scenery","expansion-river-3","River valley birds · scene 1","River scenes",1.2745098039215685],
["scenery","expansion-mountain-4","Alpine lake · scene 2","Mountain scenes",1.6049382716049383],
["scenery","expansion-river-4","Meandering river · scene 2","River scenes",1.3131313131313131],
["scenery","expansion-mountain-5","Pine ridge · scene 2","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-5","Riverside sailing · scene 2","River scenes",1.2871287128712872],
["scenery","expansion-mountain-6","Summit campsite · scene 2","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-6","Woodland river camp · scene 2","River scenes",1.2871287128712872],
["scenery","expansion-mountain-7","Highland panorama · scene 2","Mountain scenes",1.5476190476190477],
["scenery","expansion-river-7","River valley birds · scene 2","River scenes",1.2745098039215685],
["scenery","expansion-mountain-8","Alpine lake · scene 3","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-8","Meandering river · scene 3","River scenes",1.2871287128712872],
["scenery","expansion-mountain-9","Pine ridge · scene 3","Mountain scenes",1.5853658536585367],
["scenery","expansion-river-9","Riverside sailing · scene 3","River scenes",1.3],
["scenery","expansion-mountain-10","Summit campsite · scene 3","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-10","Woodland river camp · scene 3","River scenes",1.2871287128712872],
["scenery","expansion-mountain-11","Highland panorama · scene 3","Mountain scenes",1.5476190476190477],
["scenery","expansion-river-11","River valley birds · scene 3","River scenes",1.2745098039215685],
["scenery","expansion-mountain-12","Alpine lake · scene 4","Mountain scenes",1.6049382716049383],
["scenery","expansion-river-12","Meandering river · scene 4","River scenes",1.3131313131313131],
["scenery","expansion-mountain-13","Pine ridge · scene 4","Mountain scenes",1.5853658536585367],
["scenery","expansion-river-13","Riverside sailing · scene 4","River scenes",1.3],
["scenery","expansion-mountain-14","Summit campsite · scene 4","Mountain scenes",1.5662650602409638],
["scenery","expansion-river-14","Woodland river camp · scene 4","River scenes",1.2871287128712872],
["scenery","expansion-mountain-15","Highland panorama · scene 4","Mountain scenes",1.5476190476190477],
["scenery","expansion-river-15","River valley birds · scene 4","River scenes",1.2745098039215685],
["portraits","expansion-portrait-0","Cat woodland portrait","Animal portraits",1.1191024522181026],
["portraits","expansion-portrait-1","Dog woodland portrait","Animal portraits",1.0844252774286203],
["portraits","expansion-portrait-2","Lion woodland portrait","Animal portraits",1.0000000000000002],
["portraits","expansion-portrait-3","Tiger woodland portrait","Animal portraits",1.1191024522181026],
["portraits","expansion-portrait-4","Fox woodland portrait","Animal portraits",1.1191024522181026],
["portraits","expansion-portrait-5","Bear woodland portrait","Animal portraits",1.1543826399250459],
["portraits","expansion-portrait-6","Panda woodland portrait","Animal portraits",1.1543826399250459],
["portraits","expansion-portrait-7","Owl woodland portrait","Animal portraits",0.8817170835657777],
["portraits","expansion-portrait-8","Rabbit woodland portrait","Animal portraits",0.5960162147507175],
["portraits","expansion-portrait-9","Mouse woodland portrait","Animal portraits",1.2148099569106676],
["portraits","expansion-portrait-10","Bat woodland portrait","Animal portraits",1.8651707536968376],
["portraits","expansion-portrait-11","Frog woodland portrait","Animal portraits",1.0010079483956495],
["portraits","expansion-portrait-12","Seal woodland portrait","Animal portraits",1.3763859290440181],
["portraits","expansion-portrait-13","Monkey woodland portrait","Animal portraits",1.1543826399250459],
["portraits","expansion-portrait-14","Koala woodland portrait","Animal portraits",1.2148099569106676],
["portraits","expansion-portrait-15","Pig woodland portrait","Animal portraits",1.1543826399250459],
["portraits","expansion-portrait-16","Cow woodland portrait","Animal portraits",0.9562221830976817],
["portraits","expansion-portrait-17","Deer woodland portrait","Animal portraits",0.7694691705394594],
["portraits","expansion-portrait-18","Wolf woodland portrait","Animal portraits",0.8817170835657777],
["portraits","expansion-portrait-19","Horse woodland portrait","Animal portraits",0.6881386477545272],
["portraits","expansion-portrait-20","Sheep woodland portrait","Animal portraits",1.1543826399250459],
["portraits","expansion-portrait-21","Goat woodland portrait","Animal portraits",0.9562221830976817],
["portraits","expansion-portrait-22","Hedgehog woodland portrait","Animal portraits",0.9980267284282716],
["portraits","expansion-portrait-23","Raccoon woodland portrait","Animal portraits",1.1543826399250459],
["bugs","expansion-insect-0-0","Ladybird · wing and body study 1","Insects",1.103448275862069],
["bugs","expansion-insect-0-1","Ladybird · wing and body study 2","Insects",1.0909090909090908],
["bugs","expansion-insect-0-2","Ladybird · wing and body study 3","Insects",1.0810810810810811],
["bugs","expansion-insect-1-0","Stag beetle · wing and body study 1","Insects",0.9552238805970149],
["bugs","expansion-insect-1-1","Stag beetle · wing and body study 2","Insects",0.96],
["bugs","expansion-insect-1-2","Stag beetle · wing and body study 3","Insects",0.963855421686747],
["bugs","expansion-insect-2-0","Dragonfly · wing and body study 1","Insects",0.958904109589041],
["bugs","expansion-insect-2-1","Dragonfly · wing and body study 2","Insects",0.95],
["bugs","expansion-insect-2-2","Dragonfly · wing and body study 3","Insects",0.9425287356321839],
["bugs","expansion-insect-3-0","Honey bee · wing and body study 1","Insects",1.145764177642621],
["bugs","expansion-insect-3-1","Honey bee · wing and body study 2","Insects",1.172959099971934],
["bugs","expansion-insect-3-2","Honey bee · wing and body study 3","Insects",1.211029654847935],
["bugs","expansion-insect-4-0","Butterfly · wing and body study 1","Insects",0.958904109589041],
["bugs","expansion-insect-4-1","Butterfly · wing and body study 2","Insects",0.95],
["bugs","expansion-insect-4-2","Butterfly · wing and body study 3","Insects",0.9425287356321839],
["bugs","expansion-insect-5-0","Grasshopper · wing and body study 1","Insects",1.6428571428571428],
["bugs","expansion-insect-5-1","Grasshopper · wing and body study 2","Insects",1.6428571428571428],
["bugs","expansion-insect-5-2","Grasshopper · wing and body study 3","Insects",1.38],
["play","expansion-toy-0","Rocking horse · craft outline","Children’s toy artwork",0.969624746910871],
["play","expansion-toy-1","Pull-along duck · craft outline","Children’s toy artwork",1.5149679835409973],
["play","expansion-toy-2","Toy locomotive · craft outline","Children’s toy artwork",1.1016949152542372],
["play","expansion-toy-3","Wooden toy aeroplane · craft outline","Children’s toy artwork",1.3],
["play","expansion-toy-4","Sailing bath boat · craft outline","Children’s toy artwork",0.9230769230769231],
["play","expansion-toy-5","Toy robot · craft outline","Children’s toy artwork",0.7764705882352941],
["play","expansion-toy-6","Teddy bear · craft outline","Children’s toy artwork",0.7764705882352941],
["play","expansion-toy-7","Spinning top · craft outline","Children’s toy artwork",0.9491525423728814],
["play","expansion-toy-8","Toy castle · craft outline","Children’s toy artwork",1.3333333333333333],
["play","expansion-toy-9","Toy sailing submarine · craft outline","Children’s toy artwork",1.6326530612244898],
["play","expansion-toy-10","Toy giraffe · craft outline","Children’s toy artwork",0.7945205479452054],
["play","expansion-toy-11","Toy dinosaur · craft outline","Children’s toy artwork",1.4230769230769231],
["play","expansion-matching-board-0","Circle matching board and tokens","Learning & play",1.2525252525252526],
["play","expansion-matching-board-1","Square matching board and tokens","Learning & play",1.3187957569222184],
["play","expansion-matching-board-2","Triangle matching board and tokens","Learning & play",1.2525252525252526],
["play","expansion-matching-board-3","Diamond matching board and tokens","Learning & play",1.2525252525252526],
["play","expansion-matching-board-4","Star matching board and tokens","Learning & play",1.3072390640432838],
["play","expansion-matching-board-5","Hexagon matching board and tokens","Learning & play",1.305263157894737],
["play","expansion-counting-card-0","3-shape counting card","Learning & play",1],
["play","expansion-counting-card-1","4-shape counting card","Learning & play",1],
["play","expansion-counting-card-2","5-shape counting card","Learning & play",1],
["play","expansion-counting-card-3","6-shape counting card","Learning & play",1],
["play","expansion-counting-card-4","7-shape counting card","Learning & play",1],
["play","expansion-counting-card-5","8-shape counting card","Learning & play",1],
["vehicles-and-music","expansion-vm-saloon","Classic four-door saloon","Cars & road vehicles",2.0869565217391304],
["vehicles-and-music","expansion-vm-sports","Low sports coupe","Cars & road vehicles",2.4],
["vehicles-and-music","expansion-vm-vintage","1930s upright touring car","Cars & road vehicles",1.9795918367346939],
["vehicles-and-music","expansion-vm-hotrod","Open hot rod with exposed engine","Cars & road vehicles",2.5],
["vehicles-and-music","expansion-vm-convertible","Open-top beach convertible","Cars & road vehicles",2.4],
["vehicles-and-music","expansion-vm-hatchback","Compact city hatchback","Cars & road vehicles",2.2325581395348837],
["vehicles-and-music","expansion-vm-estate","Long-roof estate wagon","Cars & road vehicles",2.0869565217391304],
["vehicles-and-music","expansion-vm-pickup","Single-cab utility pickup","Cars & road vehicles",1.9591836734693877],
["vehicles-and-music","expansion-vm-doublecab","Twin-cab off-road utility","Cars & road vehicles",1.8823529411764706],
["vehicles-and-music","expansion-vm-deliveryvan","High-roof delivery van","Cars & road vehicles",1.7777777777777777],
["vehicles-and-music","expansion-vm-minibus","Community passenger minibus","Cars & road vehicles",1.92],
["vehicles-and-music","expansion-vm-bus","Long-distance coach","Cars & road vehicles",2.425531914893617],
["vehicles-and-music","expansion-vm-doubledecker","City double-decker bus","Cars & road vehicles",1.4848484848484849],
["vehicles-and-music","expansion-vm-boxtruck","Box-bodied freight truck","Cars & road vehicles",1.5625],
["vehicles-and-music","expansion-vm-tanker","Road tanker with cylindrical tank","Cars & road vehicles",1.639344262295082],
["vehicles-and-music","expansion-vm-fireengine","Fire engine with roof ladder","Cars & road vehicles",1.65],
["vehicles-and-music","expansion-vm-ambulance","Emergency ambulance outline","Cars & road vehicles",1.6551724137931034],
["vehicles-and-music","expansion-vm-tractor","Farm tractor with rear mudguard","Cars & road vehicles",1.1805555555555556],
["vehicles-and-music","expansion-vm-excavator","Tracked excavator","Cars & road vehicles",1.8620689655172413],
["vehicles-and-music","expansion-vm-forklift","Warehouse forklift","Cars & road vehicles",1.2666666666666666],
["vehicles-and-music","expansion-vm-camper","Vintage camper van","Cars & road vehicles",1.6551724137931034],
["vehicles-and-music","expansion-vm-caravan","Touring caravan trailer","Cars & road vehicles",1.8679245283018868],
["vehicles-and-music","expansion-vm-formula","Open-wheel circuit racing car","Cars & road vehicles",2.3658536585365852],
["vehicles-and-music","expansion-vm-rally","Rally coupe with auxiliary lamps","Cars & road vehicles",2.1818181818181817],
["vehicles-and-music","expansion-vm-taxi","City taxi with roof sign","Cars & road vehicles",1.7777777777777777],
["vehicles-and-music","expansion-vm-roadster","Classic twin-cylinder road motorcycle","Motorbikes & cycles",1.9392812663347414],
["vehicles-and-music","expansion-vm-chopper","Long-fork custom chopper","Motorbikes & cycles",1.8881481876302932],
["vehicles-and-music","expansion-vm-sportbike","Fully faired sport motorcycle","Motorbikes & cycles",1.6432773315564104],
["vehicles-and-music","expansion-vm-dirtbike","Off-road motocross motorcycle","Motorbikes & cycles",1.6730769230769231],
["vehicles-and-music","expansion-vm-scooter","Step-through city scooter","Motorbikes & cycles",1.501762534190458],
["vehicles-and-music","expansion-vm-touringbike","Touring motorcycle with panniers","Motorbikes & cycles",1.5240689261770917],
["vehicles-and-music","expansion-vm-cafebike","Cafe racer with seat cowl","Motorbikes & cycles",1.8751773427665928],
["vehicles-and-music","expansion-vm-adventurebike","Adventure motorcycle with luggage","Motorbikes & cycles",1.45],
["vehicles-and-music","expansion-vm-bicycle","Diamond-frame road bicycle","Motorbikes & cycles",1.8],
["vehicles-and-music","expansion-vm-basketbike","Step-through bicycle with front basket","Motorbikes & cycles",1.7678571428571428],
["vehicles-and-music","expansion-vm-tandem","Two-rider tandem bicycle","Motorbikes & cycles",2.5090909090909093],
["vehicles-and-music","expansion-vm-unicycle","Circus unicycle","Motorbikes & cycles",0.676923076923077],
["vehicles-and-music","expansion-vm-classicalguitar","Nylon-string classical guitar","Musical instruments",0.5418975069252078],
["vehicles-and-music","expansion-vm-steelguitar","Steel-string dreadnought guitar","Musical instruments",0.5625],
["vehicles-and-music","expansion-vm-electricguitar","Double-cutaway electric guitar","Musical instruments",0.47238745821713246],
["vehicles-and-music","expansion-vm-bassguitar","Four-string electric bass","Musical instruments",0.45269491457922206],
["vehicles-and-music","expansion-vm-ukulele","Four-string soprano ukulele","Musical instruments",0.5756094997898277],
["vehicles-and-music","expansion-vm-banjo","Five-string resonator banjo","Musical instruments",0.4842105263157895],
["vehicles-and-music","expansion-vm-mandolin","Teardrop mandolin","Musical instruments",0.5101619996767913],
["vehicles-and-music","expansion-vm-violin","Violin with bow and f holes","Musical instruments",0.6887147484756098],
["vehicles-and-music","expansion-vm-cello","Cello with endpin","Musical instruments",0.5416199252136752],
["vehicles-and-music","expansion-vm-doublebass","Double bass with sloping shoulders","Musical instruments",0.3698009581077756],
["vehicles-and-music","expansion-vm-harp","Concert harp with curved neck","Musical instruments",0.8406463905779115],
["vehicles-and-music","expansion-vm-lyre","Ancient seven-string lyre","Musical instruments",0.8847610011245265],
["vehicles-and-music","expansion-vm-grandpiano","Grand piano from above","Musical instruments",0.7899827903388523],
["vehicles-and-music","expansion-vm-uprightpiano","Upright piano front elevation","Musical instruments",1.12],
["vehicles-and-music","expansion-vm-accordion","Piano accordion with bellows","Musical instruments",1.1673151750972763],
["vehicles-and-music","expansion-vm-concertina","Hexagonal concertina","Musical instruments",1.52],
["vehicles-and-music","expansion-vm-trumpet","Brass trumpet with three valves","Musical instruments",2.2097902097902096],
["vehicles-and-music","expansion-vm-trombone","Slide trombone","Musical instruments",3.1551724137931036],
["vehicles-and-music","expansion-vm-frenchhorn","Coiled French horn","Musical instruments",1.1109737273185485],
["vehicles-and-music","expansion-vm-tuba","Concert tuba","Musical instruments",0.752198076723422],
["vehicles-and-music","expansion-vm-saxophone","Alto saxophone with keywork","Musical instruments",0.7063582506695939],
["vehicles-and-music","expansion-vm-clarinet","Clarinet with rings and keys","Musical instruments",0.24489795918367346],
["vehicles-and-music","expansion-vm-flute","Transverse concert flute","Musical instruments",8.363636363636363],
["vehicles-and-music","expansion-vm-recorder","Wooden descant recorder","Musical instruments",0.20224719101123595],
["vehicles-and-music","expansion-vm-panpipes","Andean panpipe set","Musical instruments",0.9242424242424242],
["vehicles-and-music","expansion-vm-bagpipes","Highland bagpipes","Musical instruments",0.65],
["vehicles-and-music","expansion-vm-drumkit","Five-piece acoustic drum kit","Musical instruments",1.4177215189873418],
["vehicles-and-music","expansion-vm-snaredrum","Snare drum and crossed sticks","Musical instruments",0.9838709677419355],
["vehicles-and-music","expansion-vm-djembe","Rope-tuned djembe drum","Musical instruments",0.6153846153846154],
["vehicles-and-music","expansion-vm-tambourine","Tambourine with paired jingles","Musical instruments",1],
["vehicles-and-music","expansion-vm-xylophone","Two-mallet concert xylophone","Musical instruments",1.0142857142857142],
["vehicles-and-music","expansion-vm-maracas","Paired decorated maracas","Musical instruments",1.0331738658250054],
["vehicles-and-music","expansion-vm-handbell","Tuned handbell with clapper","Musical instruments",0.7671232876712328],
["vehicles-and-music","expansion-vm-whole-note","Semibreve whole note","Music notation",1.8],
["vehicles-and-music","expansion-vm-half-note","Minim half note","Music notation",0.3333333333333333],
["vehicles-and-music","expansion-vm-quarter-note","Crotchet quarter note","Music notation",0.3333333333333333],
["vehicles-and-music","expansion-vm-eighth-note","Quaver eighth note with curved flag","Music notation",0.6850645277235243],
["vehicles-and-music","expansion-vm-sixteenth-note","Semiquaver sixteenth note","Music notation",0.6850645277235243],
["vehicles-and-music","expansion-vm-thirtysecond-note","Demisemiquaver thirty-second note","Music notation",0.6850645277235243],
["vehicles-and-music","expansion-vm-dotted-quarter","Dotted crotchet","Music notation",0.6055555555555556],
["vehicles-and-music","expansion-vm-dotted-half","Dotted minim","Music notation",0.6055555555555556],
["vehicles-and-music","expansion-vm-pair-eighth","Pair of beamed quavers","Music notation",0.7619047619047619],
["vehicles-and-music","expansion-vm-four-eighth","Four rising beamed quavers","Music notation",1.7142857142857142],
["vehicles-and-music","expansion-vm-pair-sixteenth","Pair of beamed semiquavers","Music notation",0.7619047619047619],
["vehicles-and-music","expansion-vm-four-sixteenth","Four alternating semiquavers","Music notation",1.7142857142857142],
["vehicles-and-music","expansion-vm-sharp","Sharp accidental","Music notation",0.7222222222222222],
["vehicles-and-music","expansion-vm-flat","Flat accidental","Music notation",0.3651315789473684],
["vehicles-and-music","expansion-vm-natural","Natural accidental","Music notation",0.2608695652173913],
["vehicles-and-music","expansion-vm-double-sharp","Double sharp accidental","Music notation",1],
["vehicles-and-music","expansion-vm-whole-rest","Semibreve rest on staff line","Music notation",6],
["vehicles-and-music","expansion-vm-half-rest","Minim rest on staff line","Music notation",6],
["vehicles-and-music","expansion-vm-quarter-rest","Crotchet rest","Music notation",0.43478260869565216],
["vehicles-and-music","expansion-vm-eighth-rest","Quaver rest","Music notation",0.5454545454545454],
["vehicles-and-music","expansion-vm-sixteenth-rest","Semiquaver rest","Music notation",0.5476190476190477],
["vehicles-and-music","expansion-vm-bass-clef","Bass F clef","Music notation",0.8836140464214849],
["vehicles-and-music","expansion-vm-treble-clef","Treble G clef","Music notation",0.4246351862528238],
["vehicles-and-music","expansion-vm-alto-clef","Alto C clef","Music notation",0.8289581638766893],
["vehicles-and-music","expansion-vm-fermata","Fermata pause sign","Music notation",2],
["vehicles-and-music","expansion-vm-crescendo","Crescendo hairpin","Music notation",2.3333333333333335],
["vehicles-and-music","expansion-vm-diminuendo","Diminuendo hairpin","Music notation",2.3333333333333335],
["vehicles-and-music","expansion-vm-repeat-bars","Repeat bar lines with dots","Music notation",0.7954545454545454],
["vehicles-and-music","expansion-vm-coda","Coda navigation symbol","Music notation",1],
["vehicles-and-music","expansion-vm-segno","Segno navigation symbol","Music notation",1.0909090909090908],
["garden-flowers-0","millennium-garden-flower-0","Garden rose · blossom study","Garden flowers",1],
["garden-flowers-0","millennium-garden-spray-0","Garden rose · leafy stem","Floral sprays",0.6138474387290656],
["garden-flowers-0","millennium-garden-corner-0","Garden rose · garden corner","Floral corners",1],
["garden-flowers-1","millennium-garden-flower-1","Spring tulip · blossom study","Garden flowers",1.1111111111111112],
["garden-flowers-1","millennium-garden-spray-1","Spring tulip · leafy stem","Floral sprays",0.5],
["garden-flowers-1","millennium-garden-corner-1","Spring tulip · garden corner","Floral corners",1],
["garden-flowers-2","millennium-garden-flower-2","Field poppy · blossom study","Garden flowers",1],
["garden-flowers-2","millennium-garden-spray-2","Field poppy · leafy stem","Floral sprays",0.6274509803921569],
["garden-flowers-2","millennium-garden-corner-2","Field poppy · garden corner","Floral corners",1],
["garden-flowers-3","millennium-garden-flower-3","Oriental lily · blossom study","Garden flowers",1.1547005383792515],
["garden-flowers-3","millennium-garden-spray-3","Oriental lily · leafy stem","Floral sprays",0.7116247844681097],
["garden-flowers-3","millennium-garden-corner-3","Oriental lily · garden corner","Floral corners",1],
["garden-flowers-4","millennium-garden-flower-4","Orchid spray · blossom study","Garden flowers",0.9564691961997489],
["garden-flowers-4","millennium-garden-spray-4","Orchid spray · leafy stem","Floral sprays",0.5796653697064181],
["garden-flowers-4","millennium-garden-corner-4","Orchid spray · garden corner","Floral corners",1],
["garden-flowers-5","millennium-garden-flower-5","Bluebell cluster · blossom study","Garden flowers",1.040339702760085],
["garden-flowers-5","millennium-garden-spray-5","Bluebell cluster · leafy stem","Floral sprays",0.44545454545454544],
["garden-flowers-5","millennium-garden-corner-5","Bluebell cluster · garden corner","Floral corners",1],
["garden-flowers-6","millennium-garden-flower-6","Sunflower · blossom study","Garden flowers",0.9945218953682734],
["garden-flowers-6","millennium-garden-spray-6","Sunflower · leafy stem","Floral sprays",0.6216637300997778],
["garden-flowers-6","millennium-garden-corner-6","Sunflower · garden corner","Floral corners",1],
["garden-flowers-7","millennium-garden-flower-7","Bearded iris · blossom study","Garden flowers",0.9398850054818205],
["garden-flowers-7","millennium-garden-spray-7","Bearded iris · leafy stem","Floral sprays",0.613765328003377],
["garden-flowers-7","millennium-garden-corner-7","Bearded iris · garden corner","Floral corners",0.9508595000789564],
["garden-flowers-8","millennium-garden-flower-8","Hydrangea head · blossom study","Garden flowers",1.1645907949963954],
["garden-flowers-8","millennium-garden-spray-8","Hydrangea head · leafy stem","Floral sprays",0.6542091308691866],
["garden-flowers-8","millennium-garden-corner-8","Hydrangea head · garden corner","Floral corners",0.9877804796533561],
["garden-flowers-9","millennium-garden-flower-9","Water lotus · blossom study","Garden flowers",1],
["garden-flowers-9","millennium-garden-spray-9","Water lotus · leafy stem","Floral sprays",0.6274509803921569],
["garden-flowers-9","millennium-garden-corner-9","Water lotus · garden corner","Floral corners",1],
["garden-flowers-10","millennium-garden-flower-10","Wild rose · blossom study","Garden flowers",1.000063012667597],
["garden-flowers-10","millennium-garden-spray-10","Wild rose · leafy stem","Floral sprays",0.6138644155685448],
["garden-flowers-10","millennium-garden-corner-10","Wild rose · garden corner","Floral corners",1],
["garden-flowers-11","millennium-garden-flower-11","Parrot tulip · blossom study","Garden flowers",1.125],
["garden-flowers-11","millennium-garden-spray-11","Parrot tulip · leafy stem","Floral sprays",0.5242718446601942],
["garden-flowers-11","millennium-garden-corner-11","Parrot tulip · garden corner","Floral corners",1],
["garden-flowers-12","millennium-garden-flower-12","California poppy · blossom study","Garden flowers",0.9823904605720678],
["garden-flowers-12","millennium-garden-spray-12","California poppy · leafy stem","Floral sprays",0.6208279631647745],
["garden-flowers-12","millennium-garden-corner-12","California poppy · garden corner","Floral corners",0.996459221723935],
["garden-flowers-13","millennium-garden-flower-13","Tiger lily · blossom study","Garden flowers",1.1547005383792515],
["garden-flowers-13","millennium-garden-spray-13","Tiger lily · leafy stem","Floral sprays",0.7116247844681097],
["garden-flowers-13","millennium-garden-corner-13","Tiger lily · garden corner","Floral corners",1],
["garden-flowers-14","millennium-garden-flower-14","Moth orchid · blossom study","Garden flowers",0.9576604469254506],
["garden-flowers-14","millennium-garden-spray-14","Moth orchid · leafy stem","Floral sprays",0.6051996721568121],
["garden-flowers-14","millennium-garden-corner-14","Moth orchid · garden corner","Floral corners",0.996459221723935],
["garden-flowers-15","millennium-garden-flower-15","Foxglove bells · blossom study","Garden flowers",0.867430441898527],
["garden-flowers-15","millennium-garden-spray-15","Foxglove bells · leafy stem","Floral sprays",0.4818181818181818],
["garden-flowers-15","millennium-garden-corner-15","Foxglove bells · garden corner","Floral corners",1],
["garden-flowers-16","millennium-garden-flower-16","Autumn sunflower · blossom study","Garden flowers",1.0154266118857451],
["garden-flowers-16","millennium-garden-spray-16","Autumn sunflower · leafy stem","Floral sprays",0.6571098186397873],
["garden-flowers-16","millennium-garden-corner-16","Autumn sunflower · garden corner","Floral corners",1],
["garden-flowers-17","millennium-garden-flower-17","Siberian iris · blossom study","Garden flowers",0.9500761168546427],
["garden-flowers-17","millennium-garden-spray-17","Siberian iris · leafy stem","Floral sprays",0.6223665331937739],
["garden-flowers-17","millennium-garden-corner-17","Siberian iris · garden corner","Floral corners",0.9508548301769457],
["garden-flowers-18","millennium-garden-flower-18","Lacecap hydrangea · blossom study","Garden flowers",1.1588949431518116],
["garden-flowers-18","millennium-garden-spray-18","Lacecap hydrangea · leafy stem","Floral sprays",0.6676240679456825],
["garden-flowers-18","millennium-garden-corner-18","Lacecap hydrangea · garden corner","Floral corners",0.9782673582885886],
["garden-flowers-19","millennium-garden-flower-19","Sacred lotus · blossom study","Garden flowers",1.0514622242382672],
["garden-flowers-19","millennium-garden-spray-19","Sacred lotus · leafy stem","Floral sprays",0.6644783079819608],
["garden-flowers-19","millennium-garden-corner-19","Sacred lotus · garden corner","Floral corners",1],
["garden-flowers-20","millennium-garden-flower-20","Floribunda rose · blossom study","Garden flowers",1],
["garden-flowers-20","millennium-garden-spray-20","Floribunda rose · leafy stem","Floral sprays",0.6138011747674327],
["garden-flowers-20","millennium-garden-corner-20","Floribunda rose · garden corner","Floral corners",1],
["garden-flowers-21","millennium-garden-flower-21","Fringed tulip · blossom study","Garden flowers",1.1372549019607843],
["garden-flowers-21","millennium-garden-spray-21","Fringed tulip · leafy stem","Floral sprays",0.5471698113207547],
["garden-flowers-21","millennium-garden-corner-21","Fringed tulip · garden corner","Floral corners",1],
["garden-flowers-22","millennium-garden-flower-22","Iceland poppy · blossom study","Garden flowers",1.1099111852210723],
["garden-flowers-22","millennium-garden-spray-22","Iceland poppy · leafy stem","Floral sprays",0.7028845663459387],
["garden-flowers-22","millennium-garden-corner-22","Iceland poppy · garden corner","Floral corners",1],
["garden-flowers-23","millennium-garden-flower-23","Asiatic lily · blossom study","Garden flowers",1.1547005383792515],
["garden-flowers-23","millennium-garden-spray-23","Asiatic lily · leafy stem","Floral sprays",0.7116247844681097],
["garden-flowers-23","millennium-garden-corner-23","Asiatic lily · garden corner","Floral corners",1],
["garden-flowers-24","millennium-garden-flower-24","Cattleya orchid · blossom study","Garden flowers",0.951909963541242],
["garden-flowers-24","millennium-garden-spray-24","Cattleya orchid · leafy stem","Floral sprays",0.625328871257521],
["garden-flowers-24","millennium-garden-corner-24","Cattleya orchid · garden corner","Floral corners",0.9778273985523249],
["garden-flowers-25","millennium-garden-flower-25","Lily of the valley · blossom study","Garden flowers",0.7589880159786951],
["garden-flowers-25","millennium-garden-spray-25","Lily of the valley · leafy stem","Floral sprays",0.5181818181818182],
["garden-flowers-25","millennium-garden-corner-25","Lily of the valley · garden corner","Floral corners",1],
["garden-flowers-26","millennium-garden-flower-26","Dwarf sunflower · blossom study","Garden flowers",0.9972037971811805],
["garden-flowers-26","millennium-garden-spray-26","Dwarf sunflower · leafy stem","Floral sprays",0.6760940339780578],
["garden-flowers-26","millennium-garden-corner-26","Dwarf sunflower · garden corner","Floral corners",0.9962859854618576],
["garden-flowers-27","millennium-garden-flower-27","Dutch iris · blossom study","Garden flowers",0.9277688992732267],
["garden-flowers-27","millennium-garden-spray-27","Dutch iris · leafy stem","Floral sprays",0.6048996223562658],
["garden-flowers-27","millennium-garden-corner-27","Dutch iris · garden corner","Floral corners",0.9508917732531275],
["garden-flowers-28","millennium-garden-flower-28","Oakleaf hydrangea · blossom study","Garden flowers",1.1535801279749562],
["garden-flowers-28","millennium-garden-spray-28","Oakleaf hydrangea · leafy stem","Floral sprays",0.6807742025383389],
["garden-flowers-28","millennium-garden-corner-28","Oakleaf hydrangea · garden corner","Floral corners",0.9691182049214607],
["garden-flowers-29","millennium-garden-flower-29","Pond lotus · blossom study","Garden flowers",1],
["garden-flowers-29","millennium-garden-spray-29","Pond lotus · leafy stem","Floral sprays",0.6792452830188679],
["garden-flowers-29","millennium-garden-corner-29","Pond lotus · garden corner","Floral corners",1],
["succulents-0","millennium-garden-succulent-0","Echeveria · ceramic planter","Succulents & cacti",0.5737640910471365],
["succulents-1","millennium-garden-succulent-1","Haworthia · ceramic planter","Succulents & cacti",0.6394864662306553],
["succulents-2","millennium-garden-succulent-2","Aloe · ceramic planter","Succulents & cacti",0.5917062583627987],
["succulents-3","millennium-garden-succulent-3","Agave · ceramic planter","Succulents & cacti",0.6221743810548976],
["succulents-4","millennium-garden-succulent-4","Jade plant · ceramic planter","Succulents & cacti",0.6106212923178577],
["succulents-5","millennium-garden-succulent-5","Burro tail · ceramic planter","Succulents & cacti",0.6409130618281864],
["succulents-6","millennium-garden-succulent-6","Aeonium · ceramic planter","Succulents & cacti",0.6245450775173],
["succulents-7","millennium-garden-succulent-7","Sempervivum · ceramic planter","Succulents & cacti",0.6397449521785336],
["succulents-8","millennium-garden-succulent-8","Zebra succulent · ceramic planter","Succulents & cacti",0.636203876946633],
["succulents-9","millennium-garden-succulent-9","Paddle plant · ceramic planter","Succulents & cacti",0.6536762744349403],
["succulents-10","millennium-garden-succulent-10","Barrel cactus · ceramic planter","Succulents & cacti",0.5979381443298969],
["succulents-11","millennium-garden-succulent-11","Prickly pear · ceramic planter","Succulents & cacti",0.5227272727272727],
["succulents-12","millennium-garden-succulent-12","Saguaro · ceramic planter","Succulents & cacti",0.6373626373626373],
["succulents-13","millennium-garden-succulent-13","Moon cactus · ceramic planter","Succulents & cacti",0.6170212765957447],
["succulents-14","millennium-garden-succulent-14","Bunny ears cactus · ceramic planter","Succulents & cacti",0.5227272727272727],
["succulents-15","millennium-garden-succulent-15","Star cactus · ceramic planter","Succulents & cacti",0.58],
["succulents-16","millennium-garden-succulent-16","Fishbone cactus · ceramic planter","Succulents & cacti",1.5932988410589655],
["succulents-17","millennium-garden-succulent-17","Pincushion cactus · ceramic planter","Succulents & cacti",0.6829268292682927],
["succulents-18","millennium-garden-succulent-18","Organ pipe cactus · ceramic planter","Succulents & cacti",0.5979381443298969],
["succulents-19","millennium-garden-succulent-19","Christmas cactus · ceramic planter","Succulents & cacti",1.593408769396936],
["herbs-0","millennium-garden-herb-0","Rosemary · kitchen herb study","Herbs & spices",0.5314864953283809],
["herbs-1","millennium-garden-herb-1","Thyme · kitchen herb study","Herbs & spices",0.444937181066581],
["herbs-2","millennium-garden-herb-2","Basil · kitchen herb study","Herbs & spices",0.5897095238881983],
["herbs-3","millennium-garden-herb-3","Mint · kitchen herb study","Herbs & spices",0.3980350064497576],
["herbs-4","millennium-garden-herb-4","Sage · kitchen herb study","Herbs & spices",0.5501405982864677],
["herbs-5","millennium-garden-herb-5","Oregano · kitchen herb study","Herbs & spices",0.6691966531651781],
["herbs-6","millennium-garden-herb-6","Marjoram · kitchen herb study","Herbs & spices",0.3169969988775747],
["herbs-7","millennium-garden-herb-7","Lavender · kitchen herb study","Herbs & spices",0.393247972732678],
["herbs-8","millennium-garden-herb-8","Dill · kitchen herb study","Herbs & spices",0.8651205764479812],
["herbs-9","millennium-garden-herb-9","Fennel · kitchen herb study","Herbs & spices",0.6876245757240572],
["herbs-10","millennium-garden-herb-10","Parsley · kitchen herb study","Herbs & spices",0.5734519403688914],
["herbs-11","millennium-garden-herb-11","Coriander · kitchen herb study","Herbs & spices",0.49201837989982655],
["herbs-12","millennium-garden-herb-12","Chives · kitchen herb study","Herbs & spices",0.4505972202270439],
["herbs-13","millennium-garden-herb-13","Tarragon · kitchen herb study","Herbs & spices",0.5264088533868929],
["herbs-14","millennium-garden-herb-14","Lemon balm · kitchen herb study","Herbs & spices",0.5844103352080541],
["herbs-15","millennium-garden-herb-15","Chamomile · kitchen herb study","Herbs & spices",0.46768850850256993],
["herbs-16","millennium-garden-herb-16","Anise · kitchen herb study","Herbs & spices",0.7956795020920918],
["herbs-17","millennium-garden-herb-17","Bay herb · kitchen herb study","Herbs & spices",0.6049407720514175],
["herbs-18","millennium-garden-herb-18","Sorrel · kitchen herb study","Herbs & spices",0.41309641089343707],
["herbs-19","millennium-garden-herb-19","Stevia · kitchen herb study","Herbs & spices",0.4873110718701377],
["orchard-0","millennium-garden-fruit-0","Pear · fruit engraving","Fruit & orchard",0.6058595173894106],
["orchard-0","millennium-garden-fruit-branch-0","Pear · orchard branch","Fruit & orchard",1.1666941040715142],
["orchard-1","millennium-garden-fruit-1","Peach · fruit engraving","Fruit & orchard",0.7968782897080906],
["orchard-1","millennium-garden-fruit-branch-1","Peach · orchard branch","Fruit & orchard",1.1946549194419243],
["orchard-2","millennium-garden-fruit-2","Orange · fruit engraving","Fruit & orchard",0.7938058139543747],
["orchard-2","millennium-garden-fruit-branch-2","Orange · orchard branch","Fruit & orchard",1.194345317271739],
["orchard-3","millennium-garden-fruit-3","Lemon · fruit engraving","Fruit & orchard",2.1568627450980395],
["orchard-3","millennium-garden-fruit-branch-3","Lemon · orchard branch","Fruit & orchard",1.293337793179045],
["orchard-4","millennium-garden-fruit-4","Strawberry · fruit engraving","Fruit & orchard",0.6751502806755256],
["orchard-4","millennium-garden-fruit-branch-4","Strawberry · orchard branch","Fruit & orchard",1.127880136306132],
["orchard-5","millennium-garden-fruit-5","Cherry pair · fruit engraving","Fruit & orchard",1.018181818181818],
["orchard-5","millennium-garden-fruit-branch-5","Cherry pair · orchard branch","Fruit & orchard",1.2294467687853337],
["orchard-6","millennium-garden-fruit-6","Pomegranate · fruit engraving","Fruit & orchard",0.7998791525391382],
["orchard-6","millennium-garden-fruit-branch-6","Pomegranate · orchard branch","Fruit & orchard",1.1931772013181183],
["orchard-7","millennium-garden-fruit-7","Fig · fruit engraving","Fruit & orchard",0.7380636035025967],
["orchard-7","millennium-garden-fruit-branch-7","Fig · orchard branch","Fruit & orchard",1.1493293153402881],
["orchard-8","millennium-garden-fruit-8","Pineapple · fruit engraving","Fruit & orchard",0.5714285714285714],
["orchard-8","millennium-garden-fruit-branch-8","Pineapple · orchard branch","Fruit & orchard",1.111515671140108],
["orchard-9","millennium-garden-fruit-9","Avocado · fruit engraving","Fruit & orchard",0.6485765124555161],
["orchard-9","millennium-garden-fruit-branch-9","Avocado · orchard branch","Fruit & orchard",1.1026827454177843],
["fungi-0","millennium-garden-fungi-0","Spotted toadstool · cluster 1","Mushrooms & fungi",0.799474030243261],
["fungi-1","millennium-garden-fungi-1","Chanterelle · cluster 1","Mushrooms & fungi",1.0174110522331568],
["fungi-2","millennium-garden-fungi-2","Morel-inspired cap · cluster 1","Mushrooms & fungi",0.7630896837739761],
["fungi-3","millennium-garden-fungi-3","Woodland parasol · cluster 1","Mushrooms & fungi",1.1228070175438596],
["fungi-4","millennium-garden-fungi-4","Inkcap · cluster 1","Mushrooms & fungi",0.6873940079140758],
["fungi-5","millennium-garden-fungi-5","Spotted toadstool · cluster 2","Mushrooms & fungi",1.4048780487804877],
["fungi-6","millennium-garden-fungi-6","Chanterelle · cluster 2","Mushrooms & fungi",1.6888888888888889],
["fungi-7","millennium-garden-fungi-7","Morel-inspired cap · cluster 2","Mushrooms & fungi",1.125],
["fungi-8","millennium-garden-fungi-8","Woodland parasol · cluster 2","Mushrooms & fungi",1.4093264248704662],
["fungi-9","millennium-garden-fungi-9","Inkcap · cluster 2","Mushrooms & fungi",1.2203389830508475],
["fungi-10","millennium-garden-fungi-10","Spotted toadstool · cluster 3","Mushrooms & fungi",1.9317073170731707],
["fungi-11","millennium-garden-fungi-11","Chanterelle · cluster 3","Mushrooms & fungi",2.2666666666666666],
["fungi-12","millennium-garden-fungi-12","Morel-inspired cap · cluster 3","Mushrooms & fungi",1.546875],
["fungi-13","millennium-garden-fungi-13","Woodland parasol · cluster 3","Mushrooms & fungi",2.1139896373056994],
["fungi-14","millennium-garden-fungi-14","Inkcap · cluster 3","Mushrooms & fungi",1.6779661016949152],
["fungi-15","millennium-garden-fungi-15","Spotted toadstool · cluster 4","Mushrooms & fungi",2.575609756097561],
["fungi-16","millennium-garden-fungi-16","Chanterelle · cluster 4","Mushrooms & fungi",2.911111111111111],
["fungi-17","millennium-garden-fungi-17","Morel-inspired cap · cluster 4","Mushrooms & fungi",2],
["fungi-18","millennium-garden-fungi-18","Woodland parasol · cluster 4","Mushrooms & fungi",2.673575129533679],
["fungi-19","millennium-garden-fungi-19","Inkcap · cluster 4","Mushrooms & fungi",2.23728813559322],
["dividers-0","millennium-garden-divider-0","Blossom chain · divider 1","Botanical dividers",3.287841191066997],
["dividers-1","millennium-garden-divider-1","Herb garland · divider 1","Botanical dividers",3.164212243008381],
["dividers-2","millennium-garden-divider-2","Scalloped garden edging · divider 1","Botanical dividers",3.0285714285714285],
["dividers-3","millennium-garden-divider-3","Trailing leaf scroll · divider 1","Botanical dividers",3.2299959884334752],
["dividers-4","millennium-garden-divider-4","Berry sprig · divider 1","Botanical dividers",2.9654962664413502],
["dividers-5","millennium-garden-divider-5","Blossom chain · divider 2","Botanical dividers",4.022988505747127],
["dividers-6","millennium-garden-divider-6","Herb garland · divider 2","Botanical dividers",4.132891546988675],
["dividers-7","millennium-garden-divider-7","Scalloped garden edging · divider 2","Botanical dividers",4],
["dividers-8","millennium-garden-divider-8","Trailing leaf scroll · divider 2","Botanical dividers",4.262978688267098],
["dividers-9","millennium-garden-divider-9","Berry sprig · divider 2","Botanical dividers",3.9166931820923496],
["dividers-10","millennium-garden-divider-10","Blossom chain · divider 3","Botanical dividers",5.343980343980344],
["dividers-11","millennium-garden-divider-11","Herb garland · divider 3","Botanical dividers",5.180254754481808],
["dividers-12","millennium-garden-divider-12","Scalloped garden edging · divider 3","Botanical dividers",4.9714285714285715],
["dividers-13","millennium-garden-divider-13","Trailing leaf scroll · divider 3","Botanical dividers",5.298273512560536],
["dividers-14","millennium-garden-divider-14","Berry sprig · divider 3","Botanical dividers",4.867890097743349],
["dividers-15","millennium-garden-divider-15","Blossom chain · divider 4","Botanical dividers",5.977011494252874],
["dividers-16","millennium-garden-divider-16","Herb garland · divider 4","Botanical dividers",6.163601654724969],
["dividers-17","millennium-garden-divider-17","Scalloped garden edging · divider 4","Botanical dividers",5.942857142857143],
["dividers-18","millennium-garden-divider-18","Trailing leaf scroll · divider 4","Botanical dividers",6.333568336853975],
["dividers-19","millennium-garden-divider-19","Berry sprig · divider 4","Botanical dividers",5.8190870133943475],
["dividers-20","millennium-garden-divider-20","Blossom chain · divider 5","Botanical dividers",6.954022988505748],
["dividers-21","millennium-garden-divider-21","Herb garland · divider 5","Botanical dividers",7.290895545870616],
["dividers-22","millennium-garden-divider-22","Scalloped garden edging · divider 5","Botanical dividers",6.914285714285715],
["dividers-23","millennium-garden-divider-23","Trailing leaf scroll · divider 5","Botanical dividers",7.368863161147413],
["dividers-24","millennium-garden-divider-24","Berry sprig · divider 5","Botanical dividers",6.770283929045347],
["tree-0","millennium-tg-oak-study","English oak · botanical engraving","Trees",0.944712710748107],
["tree-0","millennium-tg-oak-scene","English oak · riverbank","Trees",1.5011320011812186],
["tree-1","millennium-tg-elm-study","Arching elm · botanical engraving","Trees",0.9413354996289256],
["tree-1","millennium-tg-elm-scene","Arching elm · garden bench","Trees",1.963697141015573],
["tree-2","millennium-tg-maple-study","Sugar maple · botanical engraving","Trees",0.9705039212677334],
["tree-2","millennium-tg-maple-scene","Sugar maple · mountain sunrise","Trees",2.1085680123047768],
["tree-3","millennium-tg-beech-study","European beech · botanical engraving","Trees",0.94397125171096],
["tree-3","millennium-tg-beech-scene","European beech · meadow","Trees",1.9916916733671257],
["tree-4","millennium-tg-chestnut-study","Horse chestnut · botanical engraving","Trees",0.9362926722432154],
["tree-4","millennium-tg-chestnut-scene","Horse chestnut · country gateway","Trees",1.7746478873239437],
["tree-5","millennium-tg-birch-study","Silver birch · botanical engraving","Trees",0.6527988121948504],
["tree-5","millennium-tg-birch-scene","Silver birch · riverbank","Trees",1.368072031367511],
["tree-6","millennium-tg-aspen-study","Quaking aspen · botanical engraving","Trees",0.6368285284001238],
["tree-6","millennium-tg-aspen-scene","Quaking aspen · garden bench","Trees",1.6983091132276387],
["tree-7","millennium-tg-poplar-study","Lombardy poplar · botanical engraving","Trees",0.6357658344569604],
["tree-7","millennium-tg-poplar-scene","Lombardy poplar · mountain sunrise","Trees",1.7727062800440847],
["tree-8","millennium-tg-eucalyptus-study","River red gum · botanical engraving","Trees",0.944712710748107],
["tree-8","millennium-tg-eucalyptus-scene","River red gum · meadow","Trees",1.9887622543587153],
["tree-9","millennium-tg-acacia-study","Acacia · botanical engraving","Trees",0.944535969247666],
["tree-9","millennium-tg-acacia-scene","Acacia · country gateway","Trees",1.7746478873239437],
["tree-10","millennium-tg-pine-study","Scots pine · botanical engraving","Trees",0.6684210526315789],
["tree-10","millennium-tg-pine-scene","Scots pine · riverbank","Trees",1.4558472553699282],
["tree-11","millennium-tg-spruce-study","Blue spruce · botanical engraving","Trees",0.7138157894736842],
["tree-11","millennium-tg-spruce-scene","Blue spruce · garden bench","Trees",1.8827160493827158],
["tree-12","millennium-tg-fir-study","Silver fir · botanical engraving","Trees",0.7344497607655502],
["tree-12","millennium-tg-fir-scene","Silver fir · mountain sunrise","Trees",1.9745222929936304],
["tree-13","millennium-tg-cedar-study","Cedar of Lebanon · botanical engraving","Trees",0.8157894736842105],
["tree-13","millennium-tg-cedar-scene","Cedar of Lebanon · meadow","Trees",1.9112696568951781],
["tree-14","millennium-tg-cypress-study","Italian cypress · botanical engraving","Trees",0.2631578947368421],
["tree-14","millennium-tg-cypress-scene","Italian cypress · country gateway","Trees",1.7746478873239437],
["tree-15","millennium-tg-willow-study","Weeping willow · botanical engraving","Trees",1.1237675043029183],
["tree-15","millennium-tg-willow-scene","Weeping willow · riverbank","Trees",1.6653303221288764],
["tree-16","millennium-tg-palm-study","Date palm · botanical engraving","Trees",0.9583964062671274],
["tree-16","millennium-tg-palm-scene","Date palm · garden bench","Trees",1.8675669063680445],
["tree-17","millennium-tg-coconut-study","Coconut palm · botanical engraving","Trees",0.9661233120919173],
["tree-17","millennium-tg-coconut-scene","Coconut palm · mountain sunrise","Trees",1.973417269005442],
["tree-18","millennium-tg-baobab-study","African baobab · botanical engraving","Trees",1.0169606728311755],
["tree-18","millennium-tg-baobab-scene","African baobab · meadow","Trees",2.056186209036542],
["tree-19","millennium-tg-dragon-study","Dragon blood tree · botanical engraving","Trees",1.037026528585615],
["tree-19","millennium-tg-dragon-scene","Dragon blood tree · country gateway","Trees",1.7746478873239437],
["tree-20","millennium-tg-apple-study","Apple orchard tree · botanical engraving","Trees",0.9280491325107931],
["tree-20","millennium-tg-apple-scene","Apple orchard tree · riverbank","Trees",1.5063593323205933],
["tree-21","millennium-tg-pear-study","Pear orchard tree · botanical engraving","Trees",0.9545971161174381],
["tree-21","millennium-tg-pear-scene","Pear orchard tree · garden bench","Trees",1.9786618561028015],
["tree-22","millennium-tg-cherry-study","Flowering cherry · botanical engraving","Trees",0.9280491325107931],
["tree-22","millennium-tg-cherry-scene","Flowering cherry · mountain sunrise","Trees",2.0670121366474477],
["tree-23","millennium-tg-magnolia-study","Magnolia · botanical engraving","Trees",0.9413354996289256],
["tree-23","millennium-tg-magnolia-scene","Magnolia · meadow","Trees",1.9933753927140494],
["tree-24","millennium-tg-bamboo-study","Bamboo grove · botanical engraving","Trees",0.765376765084492],
["tree-24","millennium-tg-bamboo-scene","Bamboo grove · country gateway","Trees",1.7746478873239437],
["grain-wheat","millennium-tg-wheat-study","Bread wheat · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-wheat","millennium-tg-wheat-sheaf","Bread wheat · tied harvest sheaf","Wheat & grain",0.7412635546647829],
["grain-wheat","millennium-tg-wheat-wreath","Bread wheat · open harvest wreath","Wheat & grain",0.8708251766400116],
["grain-wheat","millennium-tg-wheat-field","Bread wheat · sunrise field border","Wheat & grain",1.8141485408391],
["grain-wheat","millennium-tg-wheat-basket","Bread wheat · harvest basket emblem","Wheat & grain",0.9534135916857207],
["grain-durum","millennium-tg-durum-study","Durum wheat · botanical ear study","Wheat & grain",0.5443342736277298],
["grain-durum","millennium-tg-durum-sheaf","Durum wheat · tied harvest sheaf","Wheat & grain",0.7369208895535524],
["grain-durum","millennium-tg-durum-wreath","Durum wheat · open harvest wreath","Wheat & grain",0.8323859848302683],
["grain-durum","millennium-tg-durum-field","Durum wheat · sunrise field border","Wheat & grain",1.8141485408391],
["grain-durum","millennium-tg-durum-basket","Durum wheat · harvest basket emblem","Wheat & grain",0.8723289561195072],
["grain-barley","millennium-tg-barley-study","Awned barley · botanical ear study","Wheat & grain",0.5443342736277298],
["grain-barley","millennium-tg-barley-sheaf","Awned barley · tied harvest sheaf","Wheat & grain",0.8464887378331881],
["grain-barley","millennium-tg-barley-wreath","Awned barley · open harvest wreath","Wheat & grain",0.8272546462965983],
["grain-barley","millennium-tg-barley-field","Awned barley · sunrise field border","Wheat & grain",1.8141485408391],
["grain-barley","millennium-tg-barley-basket","Awned barley · harvest basket emblem","Wheat & grain",0.8593460197226788],
["grain-rye","millennium-tg-rye-study","Rye · botanical ear study","Wheat & grain",0.5390859031423254],
["grain-rye","millennium-tg-rye-sheaf","Rye · tied harvest sheaf","Wheat & grain",0.700571676993512],
["grain-rye","millennium-tg-rye-wreath","Rye · open harvest wreath","Wheat & grain",0.8341937907713093],
["grain-rye","millennium-tg-rye-field","Rye · sunrise field border","Wheat & grain",1.8141485408391],
["grain-rye","millennium-tg-rye-basket","Rye · harvest basket emblem","Wheat & grain",0.8789005073571762],
["grain-oats","millennium-tg-oats-study","Oats · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-oats","millennium-tg-oats-sheaf","Oats · tied harvest sheaf","Wheat & grain",0.7792507801484265],
["grain-oats","millennium-tg-oats-wreath","Oats · open harvest wreath","Wheat & grain",0.8708251766400116],
["grain-oats","millennium-tg-oats-field","Oats · sunrise field border","Wheat & grain",1.8141485408391],
["grain-oats","millennium-tg-oats-basket","Oats · harvest basket emblem","Wheat & grain",0.9503374098978087],
["grain-rice","millennium-tg-rice-study","Rice · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-rice","millennium-tg-rice-sheaf","Rice · tied harvest sheaf","Wheat & grain",0.7856749042427418],
["grain-rice","millennium-tg-rice-wreath","Rice · open harvest wreath","Wheat & grain",0.8708251766400116],
["grain-rice","millennium-tg-rice-field","Rice · sunrise field border","Wheat & grain",1.8141485408391],
["grain-rice","millennium-tg-rice-basket","Rice · harvest basket emblem","Wheat & grain",0.9503374098978087],
["grain-millet","millennium-tg-millet-study","Pearl millet · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-millet","millennium-tg-millet-sheaf","Pearl millet · tied harvest sheaf","Wheat & grain",0.7412635546647829],
["grain-millet","millennium-tg-millet-wreath","Pearl millet · open harvest wreath","Wheat & grain",0.8696668452954711],
["grain-millet","millennium-tg-millet-field","Pearl millet · sunrise field border","Wheat & grain",1.8141485408391],
["grain-millet","millennium-tg-millet-basket","Pearl millet · harvest basket emblem","Wheat & grain",0.9484555502586205],
["grain-sorghum","millennium-tg-sorghum-study","Sorghum · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-sorghum","millennium-tg-sorghum-sheaf","Sorghum · tied harvest sheaf","Wheat & grain",0.7677166127400257],
["grain-sorghum","millennium-tg-sorghum-wreath","Sorghum · open harvest wreath","Wheat & grain",0.8658746258224829],
["grain-sorghum","millennium-tg-sorghum-field","Sorghum · sunrise field border","Wheat & grain",1.8141485408391],
["grain-sorghum","millennium-tg-sorghum-basket","Sorghum · harvest basket emblem","Wheat & grain",0.9380423826548651],
["grain-buckwheat","millennium-tg-buckwheat-study","Buckwheat · botanical ear study","Wheat & grain",0.6546317535906715],
["grain-buckwheat","millennium-tg-buckwheat-sheaf","Buckwheat · tied harvest sheaf","Wheat & grain",0.9287260435733162],
["grain-buckwheat","millennium-tg-buckwheat-wreath","Buckwheat · open harvest wreath","Wheat & grain",0.8708251766400116],
["grain-buckwheat","millennium-tg-buckwheat-field","Buckwheat · sunrise field border","Wheat & grain",1.8277023543392603],
["grain-buckwheat","millennium-tg-buckwheat-basket","Buckwheat · harvest basket emblem","Wheat & grain",0.9534135916857207],
["grain-quinoa","millennium-tg-quinoa-study","Quinoa · botanical ear study","Wheat & grain",0.5927195423946391],
["grain-quinoa","millennium-tg-quinoa-sheaf","Quinoa · tied harvest sheaf","Wheat & grain",0.8137501028195586],
["grain-quinoa","millennium-tg-quinoa-wreath","Quinoa · open harvest wreath","Wheat & grain",0.8708251766400116],
["grain-quinoa","millennium-tg-quinoa-field","Quinoa · sunrise field border","Wheat & grain",1.8141485408391],
["grain-quinoa","millennium-tg-quinoa-basket","Quinoa · harvest basket emblem","Wheat & grain",0.9534135916857207]
];
// END GENERATED TEMPLATE INDEX

const templates = templateIndex.map(([recipe, id, name, category, aspect]) => ({
    id, name, category, get paths() { return getPaths(recipe, id); }, aspect, builtin: true
}));
const api = {templates, normalize};
root.CNCTemplates = api;
if (typeof module !== 'undefined') module.exports = {...api, buildCatalog};
})(globalThis);
