/* Original floristry illustrations. Each leaf, petal and cup has an occlusion
 * contour. Stems and petals hidden behind it are removed from the toolpaths. */
(function(root) {
'use strict';
const tau=2*Math.PI;
const mix=(a,b,t)=>[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t];
const seq=(n,fn)=>Array.from({length:n+1},(_,i)=>fn(i/n));
const closed=p=>[...p,p[0].slice()];
const bez=(a,b,c,n=8)=>seq(n,t=>mix(mix(a,b,t),mix(b,c,t),t));
const oval=(x,y,rx,ry,n=20)=>closed(seq(n,t=>[x+rx*Math.cos(t*tau),y+ry*Math.sin(t*tau)]).slice(0,-1));
const place=(x,y,angle,size)=>([px,py])=>[x+size*(px*Math.cos(angle)-py*Math.sin(angle)),y+size*(px*Math.sin(angle)+py*Math.cos(angle))];
const bounds=p=>p.reduce(([x,y,X,Y],[a,b])=>[Math.min(x,a),Math.min(y,b),Math.max(X,a),Math.max(Y,b)],[Infinity,Infinity,-Infinity,-Infinity]);
const overlap=(a,b)=>a[0]<=b[2]&&b[0]<=a[2]&&a[1]<=b[3]&&b[1]<=a[3];
function page() {
    const clip=typeof module!=='undefined'?require('./ornaments.js').clip:root.CNCOrnaments.clip;
    let ink=[];
    const draw=p=>{if(p.length>1) ink.push({path:p,box:bounds(p)});};
    const face=(p,details=[])=>{
        const box=bounds(p),next=[];
        for(const line of ink){
            if(!overlap(box,line.box)) next.push(line);
            else for(const run of clip([line.path],p)) next.push({path:run,box:bounds(run)});
        }
        ink=next;draw(p);details.forEach(draw);
    };
    // Batch the stems below all foliage, including stems added by later sprays.
    const layers=[[],[],[],[]];
    return {
        level:0,
        draw(p){layers[this.level].push(()=>draw(p));},
        face(p,details){layers[this.level].push(()=>face(p,details));},
        paths(){for(const layer of layers) for(const paint of layer) paint();return ink.map(line=>line.path);}
    };
}

function leaf(scene, x,y, length,width, angle, kind='smooth') {
    const tr=place(x,y,angle,1), serrate=kind==='saw', round=kind==='coin';
    const mid=t=>length*.1*Math.sin(t*Math.PI);
    const side=(t,sign)=>tr([t*length,mid(t)+sign*width*Math.sin(Math.PI*t)**(round?.52:.88)
        *(serrate?1+.19*Math.cos(10*Math.PI*t):1)]);
    const a=seq(serrate?24:10,t=>side(t,-1)), b=seq(serrate?24:10,t=>side(1-t,1));
    const mask=closed([...a,...b.slice(1,-1)]);
    const veins=[seq(8,t=>{const u=t*.9;return tr([u*length,mid(u)]);})];
    if(width>2.8) for(const t of [.32,.55])
        for(const s of [-1,1]) veins.push([tr([t*length,mid(t)]),mix(tr([(t+.17)*length,mid(t+.17)]),side(t+.17,s),.83)]);
    scene.face(mask,veins);
}

// Local meridian extends along +X. Distinct petal contours define each flower.
const petalShapes={
    oval:[[0,0],[1,-.16],[3,-.85],[6,-1],[8,-.65],[9,0],[8,.66],[6,1],[3,.81],[1,.25]],
    pointed:[[0,0],[3,-.65],[6,-.82],[9,-.40],[11,0],[8,.65],[5,.92],[2,.51]],
    notched:[[0,0],[2,-.38],[5,-.88],[8,-1],[10,-.54],[9.1,-.08],[10,.42],[8,1],[5,.88],[2,.40]],
    frilled:[[0,0],[2,-.28],[5,-.7],[8,-.95],[9.3,-.8],[10,-.93],[10.5,-.58],[10.1,-.38],[10.8,-.14],[10.2,.1],[10.8,.45],[10,.7],[9.3,.63],[8.4,1],[5,.76],[2,.31]],
    broad:[[0,0],[2,-.59],[5,-1],[8,-.92],[9.7,-.6],[10.2,-.1],[10,.43],[8.5,.86],[6,1],[3,.8],[1,.4]],
};
function petal(scene,x,y,angle,len,width,kind,vein) {
    const points=petalShapes[kind],den=Math.max(...points.map(p=>p[0])),tr=place(x,y,angle,1);
    // Round the joins on the petal shoulder while retaining the drawn cleft or tip.
    const smooth=[];
    for(let i=0;i<points.length;i++){
        const prev=points[(i+points.length-1)%points.length], p=points[i], next=points[(i+1)%points.length];
        if(i===0||(kind==='pointed'&&p[0]===den)) {smooth.push(p);continue;}
        smooth.push(mix(p,prev,.20),mix(p,next,.20));
    }
    const contour=closed(smooth.map(([a,b])=>tr([a*len/den,b*width])));
    const details=vein ? [bez(tr([.13*len,0]),tr([len*.45,.1*width]),tr([len*.73,0]),5)] : [];
    scene.face(contour,details);
}
const flowers={
    cosmos:[8,1,.32,'notched',.19], anemone:[7,1,.44,'oval',.22], dogrose:[5,1,.64,'notched',.18],
    hellebore:[5,1,.59,'pointed',.22], poppy:[4,1,.80,'frilled',.19], marigold:[11,2,.32,'frilled',.16],
    flannel:[9,1,.26,'pointed',.23], paperdaisy:[13,2,.19,'pointed',.20], wax:[5,1,.52,'broad',.26],
    dahlia:[10,2,.29,'pointed',.19], camellia:[7,2,.52,'broad',.13], aster:[12,1,.24,'oval',.23],
    buttercup:[5,1,.50,'oval',.18], cornflower:[9,1,.28,'frilled',.24], primrose:[5,1,.56,'notched',.15],
};
function openFlower(scene,type,x,y,r,angle) {
    const [count,layers,width,kind,core]=flowers[type];
    for(let level=0;level<layers;level++)
        for(let j=0;j<count;j++){
            const a=angle+tau*(j+level*.51)/count, len=r*(1-level*.27)*(1+.043*Math.sin(j*2.39));
            petal(scene,x,y,a,len,len*width,kind,level===layers-1&&count<=9);
        }
    const rr=r*core;
    scene.face(oval(x,y,rr,rr*.95));
    if(core>.2) for(let i=0;i<6;i++){
        const a=i*tau/6;scene.draw([[x+rr*.43*Math.cos(a),y+rr*.43*Math.sin(a)],[x+rr*.72*Math.cos(a+.14),y+rr*.72*Math.sin(a+.14)]]);
    } else scene.draw(oval(x,y,rr*.43,rr*.43,10));
}

function sphere(scene,x,y,r,fluff) {
    const mask=closed(seq(46,t=>{
        const a=t*tau,rad=r*(fluff?1+.064*Math.cos(14*a):1);
        return [x+rad*Math.cos(a),y+rad*Math.sin(a)];
    }).slice(0,-1));
    const details=[];
    for(let j=0;j<8;j++){
        const a=j*2.4,d=.22+.52*j/8,px=x+d*r*Math.cos(a),py=y+d*r*Math.sin(a);
        details.push(bez([px-.12*r,py],[px,py-.22*r],[px+.12*r,py-.04*r],3));
    }
    scene.face(mask,details);
}
function brush(scene,x,y,r,angle,kind) {
    const tr=place(x,y,angle,r);
    if(kind==='gum'){
        // Stamen tips fan out above the cup. Stems terminate where they emerge.
        for(let i=0;i<17;i++){
            const a=-Math.PI*.92+i*Math.PI*.85/16,reach=.83+.15*Math.cos(2.45*i);
            const base=tr([-.30+.60*i/16,.06]), end=tr([reach*Math.cos(a),reach*Math.sin(a)-.03]);
            scene.draw(bez(base,tr([.58*Math.cos(a),.79*Math.sin(a)]),end,7));
            scene.draw(oval(end[0],end[1],r*.035,r*.035,8));
        }
        scene.face(closed([[-.38,.07],[-.26,.32],[-.10,.44],[.14,.43],[.29,.28],[.37,.06]].map(tr)),
            [bez(tr([-.25,.16]),tr([0,.24]),tr([.26,.14]),6)]);
    } else if(kind==='banksia'){
        const capsule=closed(seq(34,t=>{const a=t*tau;
            return tr([.47*Math.sign(Math.cos(a))*Math.abs(Math.cos(a))**.62, Math.sign(Math.sin(a))*Math.abs(Math.sin(a))**.65]);}).slice(0,-1));
        scene.face(capsule);
        for(let row=0;row<8;row++){
            const h=-.78+.215*row;
            for(let j=0;j<3;j++){
                const u=-.29+.24*j+(row%2)*.035;
                scene.draw(bez(tr([u-.07,h]),tr([u,h-.18]),tr([u+.12,h-.02]),4));
            }
        }
    } else if(kind==='lavender'){
        for(let i=0;i<5;i++){
            for(const side of [-1,1]){
                const [cx,cy]=tr([0,.54-.31*i]);
                petal(scene,cx,cy,angle-Math.PI/2+side*(.71-.09*i),r*(.51-.055*i),r*.12,'pointed',false);
            }
        }
    } else if(kind==='paw'){
        for(let i=0;i<4;i++){
            const u=i*.22, local=[[-.26,-u+.45],[-.25,-u+.23],[.13+i*.035,-u-.29],[.38+i*.018,-u-.27],[.31,-u-.15],[.40,-u-.14],[.23,-u-.04]];
            scene.face(closed(local.map(tr)));
        }
    } else if(kind==='umbel'){
        for(let i=0;i<9;i++){
            const a=-Math.PI*.94+i*Math.PI*.88/8;
            const tip=tr([.98*Math.cos(a),.85*Math.sin(a)]);
            scene.draw([tr([0,.45]),tip]);
            for(const d of [-.34,.34]){
                const l=a+d, p=[tip[0]+r*.19*Math.cos(l),tip[1]+r*.19*Math.sin(l)];
                scene.draw([tip,p]);scene.draw(oval(p[0],p[1],r*.055,r*.055,8));
            }
        }
    } else if(kind==='bell'){
        scene.face(closed([[-.05,-.65],[-.26,-.55],[-.36,-.25],[-.38,.27],[-.62,.60],[-.32,.53],[-.11,.73],[.08,.54],[.28,.72],[.42,.46],[.61,.52],[.37,.22],[.27,-.35],[.15,-.58]].map(tr)),
            [bez(tr([.04,-.47]),tr([.17,0]),tr([.15,.42]),5)]);
    }
}
const leaves={
    willowy:[17,1.85,'smooth'], eucalyptus:[10,4.5,'coin'], teeth:[15,3.2,'saw'],
    blade:[18,1.2,'smooth'], garden:[13,3.7,'smooth'], herb:[8,2.15,'smooth'], fern:[14,2.1,'saw'],
};
function spray(scene, type, foliage, base, tip, bend, radius, pairs, rotation) {
    const mid=[(base[0]+tip[0])/2+bend,(base[1]+tip[1])/2-4];
    const at=t=>mix(mix(base,mid,t),mix(mid,tip,t),t);
    const up=Math.atan2(tip[1]-base[1],tip[0]-base[0]);
    const [len,w,kind]=leaves[foliage];
    scene.level=0;scene.draw(seq(22,at));scene.level=1;
    for(let j=0;j<pairs;j++){
        const t=.21+.52*j/Math.max(1,pairs-1);
        for(const side of [-1,1]){
            const p=at(t+(side===1?.045:0)), scale=(1-.30*t)*Math.min(1.22,radius/10);
            leaf(scene,...p,len*scale,w*scale,up+side*(.93-.18*t),kind);
        }
    }
    scene.level=2;
    if(flowers[type]) openFlower(scene,type,...tip,radius,rotation);
    else if(type==='button') sphere(scene,...tip,radius*.72,false);
    else if(type==='wattle'){
        for(const [dx,dy,sz] of [[-.54,-.10,.41],[.46,-.24,.42],[.03,-.72,.45],[.08,.18,.39]]){
            scene.draw([tip,[tip[0]+dx*radius,tip[1]+dy*radius]]);
            sphere(scene,tip[0]+dx*radius,tip[1]+dy*radius,sz*radius,true);
        }
    } else if(type==='bell'){
        const apex=[tip[0]+radius*.68,tip[1]+radius*.10];
        scene.level=0;
        scene.draw(bez(tip,[tip[0]+radius*.55,tip[1]-radius*.55],apex,7));
        scene.level=2;
        brush(scene,apex[0]-radius*.65*Math.sin(rotation),apex[1]+radius*.65*Math.cos(rotation),radius,rotation,type);
    } else brush(scene,...tip,radius,rotation,type);
}

// These are explicit planting plans. Each entry chooses its heads and foliage;
// specimen plates, buttonholes, swags and hanging arrangements use different gestures.
const plans = {
    'Australian wildflower studies':[
        ['Golden wattle sprays','wattle','wattle','willowy'],['Banksia and new growth','banksia','banksia','teeth'],
        ['Scarlet gum blossom','gum','gum','eucalyptus'],['Flannel flower duet','flannel','flannel','willowy'],
        ['Paper daisy stems','paperdaisy','paperdaisy','blade'],['Kangaroo paw pair','paw','paw','blade'],
        ['Waxflower umbels','wax','wax','herb'],['Billy button pair','button','button','blade'],
        ['Wattle and gum leaves','wattle','gum','eucalyptus'],['Coastal everlasting','paperdaisy','button','willowy'],
    ],
    'Bridal buttonholes':[
        ['Camellia lapel sprig','camellia','lavender','garden'],['Blue cornflower lapel','cornflower','umbel','herb'],
        ['Daisy and eucalyptus pin','paperdaisy','wax','eucalyptus'],['Garden rose corsage','dogrose','lavender','garden'],
        ['Golden wattle buttonhole','wattle','button','blade'],['Hellebore and fern pin','hellebore','bell','fern'],
        ['White anemone posy','anemone','umbel','herb'],['Poppy pin with rounded leaves','poppy','button','eucalyptus'],
        ['Aster and sage corsage','aster','wax','herb'],['Primrose jacket posy','primrose','bell','garden'],
    ],
    'Hand-tied bouquets':[
        ['Summer cosmos sheaf','cosmos','cornflower','blade'],['Field poppy bouquet','poppy','umbel','fern'],
        ['Everlasting flower bunch','paperdaisy','button','blade'],['Garden party bouquet','dahlia','camellia','garden'],
        ['Spring market bunch','primrose','buttercup','herb'],['Gum and waxflower bouquet','gum','wax','eucalyptus'],
        ['Anemone wedding sheaf','anemone','hellebore','garden'],['Cottage rose bunch','dogrose','bell','garden'],
        ['Wild marigold posy','marigold','aster','fern'],['Lavender harvest sheaf','lavender','lavender','blade'],
    ],
    'Meadow border panels':[
        ['Buttercup meadow edge','buttercup','umbel','herb'],['Cornflowers at dusk','cornflower','poppy','blade'],
        ['Windflower patch','anemone','bell','garden'],['Cosmos field edge','cosmos','cosmos','willowy'],
        ['Everlasting meadow','paperdaisy','button','blade'],['Bankside cow parsley','umbel','umbel','fern'],
        ['Wattle grassland edge','wattle','button','willowy'],['Primroses in grass','primrose','bell','garden'],
        ['Saffron marigold field','marigold','buttercup','fern'],['Lavender field edge','lavender','aster','herb'],
    ],
    'Floral place-card swags':[
        ['Dahlia place setting','dahlia','aster','garden'],['Poppy table flourish','poppy','umbel','fern'],
        ['Starry waxflower swag','wax','button','herb'],['Camellia reception swag','camellia','bell','garden'],
        ['Daisy table garland','paperdaisy','cosmos','blade'],['Blossom and gum swag','gum','wattle','eucalyptus'],
        ['Rose guest-card spray','dogrose','lavender','garden'],['Winter hellebore swag','hellebore','primrose','garden'],
        ['Cornflower name-card spray','cornflower','anemone','herb'],['Golden meadow setting','buttercup','button','blade'],
    ],
    'Crescent floral wreaths':[
        ['Waxflower crescent','wax','wattle','willowy'],['Windswept anemone wreath','anemone','cosmos','herb'],
        ['Summer dahlia crescent','dahlia','aster','fern'],['Moon garden garland','flannel','camellia','garden'],
        ['Banksia harvest crescent','banksia','button','teeth'],['Cornflower wreath','cornflower','umbel','blade'],
        ['Gum blossom arc','gum','wax','eucalyptus'],['Wild rose crescent','dogrose','bell','garden'],
        ['Primrose moon wreath','primrose','hellebore','herb'],['Lavender crescent','lavender','paperdaisy','willowy'],
    ],
    'Pressed flower mounts':[
        ['Pressed poppy sheet','poppy','poppy','herb'],['Buttercup herbarium','buttercup','buttercup','herb'],
        ['Wild carrot herbarium','umbel','umbel','fern'],['Cornflower specimen sheet','cornflower','cornflower','blade'],
        ['Pressed lavender stems','lavender','lavender','herb'],['Pressed bluebells','bell','bell','blade'],
        ['Waxflower collection sheet','wax','wax','herb'],['Dahlia study mount','dahlia','dahlia','garden'],
        ['Cosmos herbarium','cosmos','cosmos','willowy'],['Eucalyptus blossom sheet','gum','gum','eucalyptus'],
    ],
    'Bud vase engravings':[
        ['Anemone in a flask','anemone','bell','herb'],['Rancho marigold pot','marigold','aster','fern'],
        ['Tall wattle vase','wattle','gum','willowy'],['Wild rose bottle','dogrose','umbel','garden'],
        ['Daisy pottery still life','paperdaisy','button','blade'],['Field poppy decanter','poppy','cornflower','herb'],
        ['Banksia vessel','banksia','paw','teeth'],['Hellebore urn','hellebore','primrose','garden'],
        ['Cosmos bud vase','cosmos','bell','willowy'],['Camellia ginger jar','camellia','wax','eucalyptus'],
    ],
};

function ribbon(scene,x,y) {
    scene.level=3;
    const point=place(x,y,0,1), wrap=p=>closed(p.map(point));
    scene.face(wrap([[-2,1],[-6,6],[-11,8],[-8,8],[-9,11],[-4,8],[1,2]]));
    scene.face(wrap([[1,1],[5,3],[12,4],[9,6],[10,8],[4,6],[-1,2]]));
    scene.face(wrap([[-1,0],[-4,-3],[-8,-4],[-10,-3],[-9,0],[-7,2],[-3,2]]));
    scene.face(wrap([[1,0],[5,-4],[8,-5],[10,-4],[9,-1],[6,1],[3,2]]));
    scene.face(oval(x,y,2.6,1.7,12));
}
function jar(scene,x,y,w,h,type) {
    const silhouettes=[
        [[.24,0],[.24,.10],[.46,.28],[.50,.52],[.43,.91],[.35,1]],
        [[.19,0],[.19,.24],[.46,.38],[.50,.6],[.48,.9],[.36,1]],
        [[.42,0],[.36,.12],[.46,.37],[.50,.59],[.30,.92],[.34,1]],
        [[.17,0],[.17,.37],[.47,.50],[.47,.95],[.42,1]],
        [[.36,0],[.40,.10],[.49,.23],[.43,.71],[.28,1]],
    ];
    const half=silhouettes[type%5];
    const side=k=>half.map(([a,b])=>[x+k*a*w,y+b*h]);
    scene.level=3;
    const profile=closed([...side(-1),...side(1).reverse()]);
    scene.face(profile);
    if(type%3===0){
        for(const t of [.54,.65])scene.draw(bez([x-w*.40,y+h*t],[x,y+h*(t+.1)],[x+w*.40,y+h*t],9));
    } else if(type%3===1){
        for(const s of [-1,0,1])scene.draw(bez([x+s*w*.17,y+h*.55],[x+s*w*.25,y+h*.73],[x+s*w*.16,y+h*.91],7));
    } else {
        for(let j=0;j<5;j++)scene.draw(oval(x+w*(j-2)*.13,y+h*(.62+.05*Math.cos(j*Math.PI)),w*.028,h*.026,10));
    }
}
function design(category,index,kinds) {
    const scene=page(),[first,second,foliage]=kinds;
    const shift=[-3,1,3,-2,2,0,-1,4,-4,1][index];
    const bloom=(type,base,tip,bend,radius,pairs,angle=0)=>spray(scene,type,foliage,base,tip,bend,radius,pairs,angle);
    switch(category) {
    case 'Australian wildflower studies':
        bloom(first,[47,98],[39+shift,19],-17,13,3,-.15);
        bloom(second,[47,87],[73+shift,39],11,9,2,.22);
        break;
    case 'Bridal buttonholes':
        bloom(second,[46,93],[29+shift,23],-12,9,2,-.25);
        bloom(second,[49,94],[65,21+shift],6,7.5,2,.26);
        bloom(first,[48,91],[48,36],-2,14,2,.15);
        ribbon(scene,47,73);
        break;
    case 'Hand-tied bouquets':
        bloom(second,[56,92],[18,29+shift],-12,10,2,-.3);
        bloom(second,[46,94],[76,24-shift],17,10,2,.18);
        bloom(first,[48,96],[48+shift,13],-2,13,2,-.12);
        bloom(first,[54,96],[37,43+shift],-9,11,1,.31);
        bloom(second,[45,91],[64,49-shift],7,8,1,-.22);
        ribbon(scene,50,79);
        break;
    case 'Meadow border panels':
        // Uneven heights avoid a mechanical horizon along the flowering edge.
        for(const [j,[x,y,lean,r]] of [[6,39,8,8],[32,24,-4,10],[49,43,7,8],[73,18,-4,12],[91,38,5,8]].entries())
            bloom(j%2?first:second,[x+lean,71+Math.sin(j)*3],[x,y+shift*Math.cos(j)],-lean,r,2,(j-2)*.13);
        break;
    case 'Floral place-card swags':
        bloom(second,[52,58],[7,35+shift],-14,7,2,-.75);
        bloom(first,[52,58],[26,36-shift],-6,10,2,-.45);
        bloom(second,[46,58],[94,36-shift],7,7,2,.75);
        bloom(first,[46,58],[73,38+shift],7,10,2,.42);
        bloom(first,[51,57],[50+shift,43],0,8,0,.1);
        break;
    case 'Crescent floral wreaths': {
        scene.level=0;
        const arc=t=>[53+42*Math.cos(.12+3.12*t),39+40*Math.sin(.12+3.12*t)];
        scene.draw(seq(50,arc));
        for(let i=0;i<16;i++){
            const t=(i+.22)/16,p=arc(t),dir=.12+3.12*t+Math.PI/2;
            scene.level=1;
            leaf(scene,...p,8.5,2.3,dir+(i%2?.58:-.70),leaves[foliage][2]);
        }
        bloom(second,arc(.22),[86,42],2,8,1,-.32);
        bloom(first,arc(.62),[48+shift,67],-3,11,0,.36);
        bloom(second,arc(.81),[22,53+shift],-3,8.5,1,-.25);
        break;
    }
    case 'Pressed flower mounts':
        bloom(first,[40,90],[34+shift,15],-8,12,3,-.2);
        bloom(second,[43,86],[68,32+shift],9,9,2,.25);
        scene.level=3;
        const mountBase=[40,90],mountTip=[34+shift,15],mountMid=[(mountTip[0]+40)/2-8,48.5];
        for(const t of [.16,.64]) {
            const [x,y]=mix(mix(mountBase,mountMid,t),mix(mountMid,mountTip,t),t);
            const tr=place(x,y,-.2,1);
            scene.face(closed([[-5,-1.2],[5,-1.2],[5,1.2],[-5,1.2]].map(tr)));
        }
        break;
    case 'Bud vase engravings':
        bloom(second,[50,65],[30+shift,26],-9,9,1,-.2);
        bloom(first,[51,65],[54,13+shift],4,12,1,.12);
        bloom(second,[50,64],[74,33],9,7,1,.22);
        jar(scene,50,57,32+shift*1.5,29+index%4*3,index);
        break;
    }
    const rounded = scene.paths().map(p=>p.map(([x,y])=>[Math.round(x*1000)/1000,-Math.round(y*1000)/1000]));
    return rounded.map(p=>p.filter((v,j)=>!j||v[0]!==p[j-1][0]||v[1]!==p[j-1][1])).filter(p=>p.length>1);
}
function install(register,add) {
    const slug=name=>name.toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/-$/,'');
    for(const [category,items] of Object.entries(plans)) {
        register('stock-garden-'+slug(category),()=>{
            items.forEach(([name,...kinds],index)=>add('stock-garden-'+slug(name),name,category,design(category,index,kinds)));
        });
    }
}
if(typeof module!=='undefined'&&module.exports) module.exports=install;
else root.CNCStockGarden=install;
})(globalThis);
