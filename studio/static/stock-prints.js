/* Original miniature woodcut panoramas for KerfKite. Geometry is layered from
   sky to foreground. Opaque masks trim the vector strokes themselves. */
(function(root){
'use strict';
const W=156,H=76,pi=Math.PI,tau=2*pi,eps=1e-7;
const close=p=>[...p,p[0]];
const range=n=>Array.from({length:n},(_,i)=>i);
const lerp=(a,b,t)=>a.map((v,k)=>v+(b[k]-v)*t);
const cross=(a,b)=>a[0]*b[1]-a[1]*b[0];
const minus=(a,b)=>[a[0]-b[0],a[1]-b[1]];
const same=(p,q)=>Math.abs(p[0]-q[0])+Math.abs(p[1]-q[1])<eps;
const rect=(x0,y0,x1,y1)=>[[x0,y0],[x1,y0],[x1,y1],[x0,y1]];
function oval(x,y,a,b,n=36){return range(n).map(i=>[x+a*Math.cos(tau*i/n),y+b*Math.sin(tau*i/n)]);}
function bounds(points){return points.reduce(([l,b,r,t],[x,y])=>[Math.min(l,x),Math.min(b,y),Math.max(r,x),Math.max(t,y)],[Infinity,Infinity,-Infinity,-Infinity]);}
function contains(p,poly){
    let inside=false;
    for(let i=0,j=poly.length-1;i<poly.length;j=i++){
        const a=poly[i],b=poly[j];
        if((a[1]>p[1])!==(b[1]>p[1]) && p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;
    }
    return inside;
}
function trim(path,poly,keepInside,box=bounds(poly)){
    const pieces=[];let piece=[];
    const flush=()=>{if(piece.length>1)pieces.push(piece);piece=[];};
    for(let i=1;i<path.length;i++){
        const a=path[i-1],b=path[i],ab=minus(b,a);
        if(same(a,b))continue;
        const outside=Math.max(a[0],b[0])<box[0]||Math.min(a[0],b[0])>box[2]||Math.max(a[1],b[1])<box[1]||Math.min(a[1],b[1])>box[3];
        const ts=[0,1];
        if(!outside)for(let j=0;j<poly.length;j++){
            const c=poly[j],d=poly[(j+1)%poly.length],cd=minus(d,c),ac=minus(c,a),det=cross(ab,cd);
            if(Math.abs(det)<eps)continue;
            const t=cross(ac,cd)/det,u=cross(ac,ab)/det;
            if(t>eps&&t<1-eps&&u>=-eps&&u<=1+eps)ts.push(t);
        }
        ts.sort((x,y)=>x-y);
        for(let j=1;j<ts.length;j++){
            const lo=ts[j-1],hi=ts[j];if(hi-lo<eps)continue;
            if((!outside&&contains(lerp(a,b,(lo+hi)/2),poly))!==keepInside){flush();continue;}
            const start=lerp(a,b,lo);
            if(piece.length&&!same(piece.at(-1),start))flush();
            if(!piece.length)piece.push(start);
            piece.push(lerp(a,b,hi));
        }
    }
    flush();return pieces;
}
function rng(seed){
    let state=seed>>>0;
    return ()=>{state=(Math.imul(1664525,state)+1013904223)>>>0;return state/4294967296;};
}
function smooth(a,b,c,d,n=10){return range(n+1).map(i=>{
    const t=i/n,u=1-t;return [0,1].map(k=>u*u*u*a[k]+3*u*u*t*b[k]+3*u*t*t*c[k]+t*t*t*d[k]);
});}
function scenery(seed){
    const random=rng(seed);let ink=[];
    const add=(...paths)=>ink.push(...paths);
    function paint(poly,paths){
        const b=bounds(poly);
        ink=ink.flatMap(p=>trim(p,poly,false,b));
        add(...paths);
    }
    function shade(poly,count,steep){
        const [l,b,r,t]=bounds(poly),out=[];
        for(let i=0;i<count;i++){
            const x=l+(r-l)*(i+.5)/count;
            out.push(...trim([[x+(t-b)*steep,b],[x-(t-b)*steep,t]],poly,true));
        }
        return out;
    }
    function sky(sx,sy,r,weather='clear'){
        const disc=oval(sx,sy,r,r,48);
        // Sparse cloud shelves leave the paper exposed above the horizon.
        const clouds=weather==='wind'?7:weather==='mist'?4:3;
        for(let i=0;i<clouds;i++){
            const x=random()*(W-44)+4,y=H-6-i*(weather==='wind'?2.4:4.6)+random()*2;
            const w=12+random()*32;
            const shelf=smooth([x,y],[x+w*.34,y-1.7],[x+w*.73,y+1.2],[x+w,y-.1]);
            add(shelf);
            if(i%2===0)add(smooth([x+w*.2,y-1.6],[x+w*.46,y-2.4],[x+w*.64,y-.7],[x+w*.87,y-1.9],7));
        }
        paint(disc,[close(disc)]);
        if(weather!=='clear')for(let j=1;j<4;j++){
            const y=sy-r+j*(r*.30);
            const w=Math.sqrt(r*r-(y-sy)**2)*.80;
            add([[sx-w,y],[sx+w,y]]);
        }
    }
    // Each authored summit receives its own facing plane and a forked ridge.
    function ridge(anchors,style){
        const edge=[];
        for(let k=1;k<anchors.length;k++){
            const a=anchors[k-1],b=anchors[k],n=Math.max(1,Math.ceil((b[0]-a[0])/3));
            if(!edge.length)edge.push(a);
            for(let j=1;j<=n;j++){
                const p=lerp(a,b,j/n);
                if(j<n)p[1]+=(random()-.5)*(style==='mist'?.85:1.6);
                edge.push(p);
            }
        }
        const body=[...edge,[W,-3],[0,-3]],details=[edge];
        for(let k=1;k<anchors.length-1;k++){
            const peak=anchors[k],next=anchors[k+1],prev=anchors[k-1];
            if(peak[1]<=prev[1]||peak[1]<=next[1])continue;
            const fall=Math.min(peak[1]-2,Math.max(6,(peak[1]-Math.min(prev[1],next[1]))*1.19));
            const foot=[Math.min(W,peak[0]+(next[0]-peak[0])*.33+2),peak[1]-fall];
            const mid=lerp(peak,foot,.39);mid[0]-=Math.min(2,(next[0]-peak[0])*.17);
            const flank=[peak,mid,foot];details.push(flank);
            if(style==='mist')continue;
            for(let j=1;j<=Math.min(12,Math.ceil((next[0]-peak[0])/2));j++){
                const f=j/(Math.min(12,Math.ceil((next[0]-peak[0])/2))+1);
                const top=lerp(peak,next,f);
                top[1]-=(style==='snow'?2.1:.9)+random()*1.1;
                const tail=lerp(foot,next,f*.88);tail[1]-=random()*2;
                const m=lerp(top,tail,.53);m[0]+=.65;
                details.push([top,m,tail]);
            }
            if(style==='snow'){
                const ledge=lerp(prev,peak,.60);
                details.push([ledge,[ledge[0]+3,ledge[1]-.8],lerp(peak,foot,.45)]);
            }
        }
        if(style!=='mist'){
            // Broken striations follow the bedding. Their lengths shorten on the
            // illuminated slope and thicken towards the base of each cliff.
            for(let k=1;k<anchors.length;k++){
                const a=anchors[k-1],b=anchors[k],w=b[0]-a[0],drop=b[1]-a[1];
                const count=Math.max(1,Math.floor(w/(drop<0?2.2:4.1)));
                for(let j=0;j<count;j++){
                    const frac=(j+.55)/count, cap=lerp(a,b,frac);
                    const y=cap[1]-5-random()*5, shift=style==='snow'?2.5:1.3;
                    const len=2+random()*4+Math.min(6,Math.abs(drop)*.12);
                    details.push([[cap[0],y],[cap[0]+shift*.7,y-len*.45],[cap[0]+shift*.60,y-len*.64],[cap[0]+shift,y-len]]);
                }
            }
        }
        paint(body,[details[0],...details.slice(1).flatMap(p=>trim(p,body,true))]);
    }
    function hill(anchors,texture='grass'){
        const top=[];
        for(let i=1;i<anchors.length;i++){
            const a=anchors[i-1],b=anchors[i],w=(b[0]-a[0])/3;
            top.push(...smooth(a,[a[0]+w,a[1]],[b[0]-w,b[1]],b,8).slice(top.length?1:0));
        }
        const body=[...top,[W,-3],[0,-3]],hatches=[top];
        // Follow the lie of the ground; taper the strokes towards each ridge.
        const n=texture==='dune'?6:texture==='bare'?3:4;
        for(let band=0;band<n;band++){
            let x=3+random()*5;
            while(x<W-1){
                const len=(texture==='dune'?16:8)+random()*12,offset=2.4+band*(texture==='dune'?3.2:2.8);
                const stroke=top.filter(p=>p[0]>=x&&p[0]<=x+len).map(([u,v])=>[u,v-offset]);
                if(stroke.length>=2)hatches.push(stroke);
                x+=len+3+random()*7;
            }
        }
        if(texture==='grass')for(let j=0;j<22;j++){
            const x=3+random()*(W-6),idx=top.findIndex(p=>p[0]>=x);
            const y=top[idx][1]-7-random()*7;
            hatches.push([[x-.7,y+.7],[x,y],[x+.6,y+1.5]]);
        }
        paint(body,hatches);
    }
    function water(poly,shores,reflectionX){
        const [l,b,r,t]=bounds(poly),strokes=[...shores],dy=Math.max(2,(t-b)/12);
        for(let y=b+1.3;y<t;y+=dy){
            let x=l+random()*5;
            while(x<r){
                const len=2.3+random()*7+(t-y)*.12;
                const path=[[x,y],[x+len*.34,y+.19],[x+len*.80,y-.20],[x+len,y]];
                // Broken lines carve a column of light from the sun or moon.
                if(Math.abs(x+len/2-reflectionX)>2+(t-y)*.12)strokes.push(...trim(path,poly,true));
                x+=len+1.5+random()*4;
            }
        }
        paint(poly,strokes);
    }
    function sea(y,light){water(rect(0,-2,W,y),[[[0,y],[W,y]]],light);}
    function river(banks,light){
        // Interpolate complete banks, retaining a common monotone depth of field.
        const left=[],right=[];
        for(let i=1;i<banks.length;i++){
            const a=banks[i-1],b=banks[i];
            for(let j=i===1?0:1;j<=7;j++){
                const t=j/7,u=t*t*(3-2*t),x=a[0]+(b[0]-a[0])*u,y=a[1]+(b[1]-a[1])*t;
                const w=a[2]+(b[2]-a[2])*t;
                left.push([x-w,y]);right.push([x+w,y]);
            }
        }
        water([...left,...right.slice().reverse()],[left,right],light);
    }
    function pine(x,y,h){
        const p=[[0,1],[.069,.78],[.036,.79],[.12,.60],[.070,.63],[.17,.42],[.10,.46],[.22,.23],[.13,.26],[.27,.09],[.04,.13],[.028,0],[-.028,0],[-.04,.13],[-.24,.08],[-.13,.26],[-.20,.22],[-.10,.45],[-.16,.41],[-.07,.63],[-.115,.6],[-.04,.79],[-.065,.77]];
        const map=pts=>pts.map(([u,v])=>[x+u*h,y+v*h]);
        const outline=map(p);paint(outline,[close(outline),map([[0,.21],[0,.83]]),...range(4).map(i=>map([[.01,.21+i*.14],[.125-i*.018,.18+i*.14]]))]);
    }
    function trees(cluster){for(const [x,y,h] of cluster)pine(x,y,h);}
    function rocks(points){
        const traces=[close(points)],b=bounds(points);
        const crest=points.reduce((a,p)=>a[1]>p[1]?a:p);
        const foot=[b[0]+(b[2]-b[0])*.68,b[1]+.5];
        const middle=lerp(crest,foot,.55);middle[0]+=1.1;
        const seam=[crest,middle,foot];traces.push(...trim(seam,points,true));
        const shadow=rect((crest[0]+foot[0])*.50,b[1]-.1,b[2]+.1,b[3]+.1);
        for(const p of shade(points,Math.min(24,Math.ceil((b[2]-b[0])/.9)),.17))traces.push(...trim(p,shadow,true));
        for(let k=0;k<4;k++){
            const y=b[1]+(b[3]-b[1])*(.16+k*.16),x=b[0]+1+random()*2;
            const stroke=[[x,y],[x+(b[2]-b[0])*.17,y+.35],[x+(b[2]-b[0])*.30,y+.1]];
            traces.push(...trim(stroke,points,true));
        }
        paint(points,traces);
    }
    function gull(x,y,s){
        const wing=smooth([x-2.5*s,y+.5*s],[x-1.5*s,y+1.15*s],[x-.5*s,y+.75*s],[x,y],6);
        add([...wing,...smooth([x,y],[x+.8*s,y+1.3*s],[x+2.1*s,y+1.8*s],[x+3*s,y+1.45*s],6).slice(1)]);
    }
    function cabin(x,y,s){
        const map=pts=>pts.map(([u,v])=>[x+u*s,y+v*s]);
        const p=map([[0,0],[0,5],[-1.1,5],[4,10],[16,10],[21,5],[20,5],[20,0]]);
        paint(p,[close(p),...range(5).map(i=>map([[5.25+i*2.1,9.5],[8.9+i*2.1,5.75]])),...[[[-.5,5],[8.8,5],[4,9.5]],[[8.8,5],[20.5,5]],[[9,5],[9,0]],[[2,0],[2,3.2],[4.4,3.2],[4.4,0]],rect(12,1.7,15.3,3.8).concat([[12,1.7]]),[[16.8,3.8],[18.5,3.8]],[[16.8,2.6],[18.5,2.6]],[[1,4.1],[7.3,4.1]],[[5.6,2.3],[8,2.3]],[[5.6,1.2],[7.8,1.2]],[[10,1],[11.1,1]],[[12.2,1],[18,1]]].map(map)]);
    }
    function boat(x,y,s){
        const map=p=>p.map(([u,v])=>[x+u*s,y+v*s]);
        const hull=map([[-10,0],[9,0],[5,-2.5],[-6,-2.5]]);paint(hull,[close(hull),map([[-7.6,-.65],[7.2,-.65]])]);
        const mast=map([[0,0],[0,15]]);add(mast);
        for(const triangle of [[[.7,13.9],[7.9,1.4],[.7,1.4]],[[-.7,11.1],[-7.8,1.3],[-.7,1.3]]]){
            const sail=map(triangle);paint(sail,[close(sail)]);
        }
    }
    function lighthouse(x,y,s){
        const map=p=>p.map(([u,v])=>[x+s*u,y+s*v]);
        const mask=map([[-4,0],[4,0],[2.45,19],[3.6,19],[3.6,20.2],[2.3,20.2],[2.3,24],[3.2,24],[0,27],[-3.2,24],[-2.3,24],[-2.3,20.2],[-3.6,20.2],[-3.6,19],[-2.45,19]]);
        paint(mask,[close(mask),...[[[-2.3,24],[2.3,24]],[[-2.3,20.2],[2.3,20.2]],[[0,20.2],[0,24]],[[-2.8,13.7],[2.8,13.7]],[[-3.1,9.8],[3.1,9.8]],[[-.8,0],[-.8,4],[.8,4],[.8,0]],[[1.3,.8],[2.8,.8]],[[1.55,2],[2.6,2]],[[.5,15.3],[1.4,15.3],[1.4,16.7],[.5,16.7],[.5,15.3]],[[1.5,5.7],[2.6,5.7]],[[1.7,6.6],[2.5,6.6]]].map(map)]);
    }
    function cactus(x,y,s){
        const map=p=>p.map(([u,v])=>[x+s*u,y+s*v]);
        const mask=map([[-1.4,0],[-1.4,8],[-4,8],[-5.8,10],[-5.8,16],[-4.7,17],[-3.6,16],[-3.6,11.6],[-1.5,11.6],[-1.5,22],[0,24],[1.5,22],[1.5,14],[3.9,14],[3.9,18],[5,19],[6,18],[6,13],[4.5,11],[1.5,11],[1.5,0]]);
        paint(mask,[close(mask),map([[0,1],[0,21]]),map([[-.77,4],[-.77,18]]),
            map([[.80,7],[.80,21.1]]),map([[-4.88,15.5],[-4.88,10.8],[-3.92,9.6],[-2.1,9.6]]),
            map([[5.05,17.8],[5.05,13.4],[4.2,12.4],[2.1,12.4]])]);
    }
    function reeds(x,y,s){
        const mask= [[0,0],[-2.4,4],[-5.7,7],[-3.8,6.7],[-.9,3],[-2.3,10],[-1.3,11.7],[.4,3.2],[3.2,13.4],[4.1,13.8],[2,3.2],[5.1,7.3],[6.3,7.4],[2,.3]].map(([u,v])=>[x+s*u,y+s*v]);
        paint(mask,[close(mask)]);
    }
    function finish(){
        const page=rect(0,0,W,H);
        // Slightly inset artwork keeps long shoreline strokes off the framing rule.
        const window=rect(.4,.4,W-.4,H-.4);
        return [close(page),...ink.flatMap(p=>trim(p,window,true))];
    }
    return {sky,ridge,hill,sea,river,rocks,pine,trees,gull,cabin,boat,lighthouse,cactus,reeds,water,finish};
}
const plates=[
    ['east-watch','East watch at first light','Coastal woodcuts',s=>{
        s.sky(113,53,8,'mist');s.gull(73,53,.8);s.gull(80,57,.52);
        s.ridge([[0,34],[18,39],[36,33],[54,35],[74,30],[95,32],[118,26],[137,30],[156,27]],'mist');
        s.sea(28,113);
        s.hill([[0,49],[16,45],[32,40],[44,35],[54,32],[65,17],[76,13],[84,4],[99,-2],[156,-2]],'bare');
        s.lighthouse(39,36.5,.97);
        s.cabin(18,40,.61);
        s.rocks([[47,9],[59,14],[68,13],[72,15],[80,8],[87,5],[84,2],[53,2]]);
        s.boat(116,22,.63);
    }],
    ['needle-sound','Needles above the sound','Coastal woodcuts',s=>{
        s.sky(36,51,9,'wind');s.gull(94,50,.75);s.gull(99,52,.44);s.sea(30,36);
        s.rocks([[63,26],[67,34],[70,35],[73,47],[76,49],[77,57],[80,58],[84,44],[88,40],[90,31],[96,26],[94,24],[67,24]]);
        s.rocks([[100,25],[105,29],[107,38],[111,42],[114,35],[117,32],[120,26],[120,23],[103,23]]);
        s.hill([[0,12],[12,14],[25,9],[38,10],[52,3],[65,-3],[156,-3]],'grass');
        s.reeds(9,7,.83);s.reeds(23,6,.64);
        s.rocks([[125,9],[131,14],[139,13],[143,10],[142,7],[128,7]]);
    }],
    ['white-sails','White sails through the narrows','Coastal woodcuts',s=>{
        s.sky(132,55,6,'clear');
        s.ridge([[0,40],[19,51],[30,39],[44,47],[58,41],[69,44],[86,31],[101,34],[116,31],[133,40],[146,38],[156,43]],'rock');
        s.sea(29,132);
        s.boat(89,19,1.58);s.boat(48,25,.53);
        s.hill([[0,18],[8,17],[16,20],[24,16],[37,7],[49,2],[60,-3],[130,-3],[143,1],[156,6]],'grass');
        s.trees([[4,12,15],[13,12,20],[22,11,12],[149,2,10],[155,1,17]]);
    }],
    ['island-shelter','Two roofs on the tide island','Coastal woodcuts',s=>{
        s.sky(58,54,7,'mist');s.sea(33,58);
        s.rocks([[68,26],[73,31],[85,34],[92,36],[104,34],[115,32],[124,28],[135,25],[138,22],[107,20],[81,22]]);
        s.cabin(84,33,.74);s.cabin(101,31,.55);
        s.rocks([[8,12],[19,15],[29,14],[33,11],[40,8],[39,5],[17,5]]);
        s.rocks([[34,5],[47,10],[57,7],[61,3],[60,1],[39,1]]);
        s.reeds(10,6,.85);s.reeds(18,7,.66);
    }],
    ['cathedral-ice','Cathedral of ice','Alpine woodcuts',s=>{
        s.sky(127,58,7,'clear');
        s.ridge([[0,35],[18,47],[31,41],[50,58],[62,49],[76,67],[83,57],[94,61],[108,44],[127,50],[142,38],[156,43]],'snow');
        s.ridge([[0,36],[16,38],[29,31],[45,34],[65,20],[78,25],[93,23],[109,31],[132,34],[144,30],[156,33]],'rock');
        s.hill([[0,14],[19,18],[40,13],[57,15],[81,8],[109,10],[130,16],[156,22]]);
        s.trees([[5,8,20],[18,8,13],[27,11,16],[126,14,9],[137,14,13],[148,15,20],[155,12,17]]);
    }],
    ['scree-cabin','The scree-line hut','Alpine woodcuts',s=>{
        s.sky(35,62,5,'wind');
        s.ridge([[0,32],[17,39],[31,35],[52,51],[66,40],[89,63],[99,55],[108,65],[129,38],[141,46],[156,35]],'snow');
        s.hill([[0,24],[22,22],[39,26],[66,17],[90,20],[117,12],[139,16],[156,20]],'bare');
        s.cabin(37,23,1.10);
        s.rocks([[81,8],[88,14],[95,13],[101,7],[96,3],[84,4]]);
        s.rocks([[100,5],[107,8],[114,6],[117,3],[108,2]]);
        s.trees([[9,16,20],[17,17,13],[148,15,15]]);
    }],
    ['cirque-lake','Ice-blue cirque','Alpine woodcuts',s=>{
        s.sky(89,57,6,'clear');
        s.ridge([[0,61],[12,58],[24,62],[41,43],[54,50],[65,42],[82,51],[96,41],[114,54],[127,52],[140,63],[156,55]],'snow');
        s.hill([[0,40],[17,34],[31,27],[52,25],[75,23],[97,21],[120,28],[141,36],[156,42]],'bare');
        const rim=[[12,14],[25,17],[44,19],[57,20],[73,19],[91,20],[105,18],[124,15],[141,11],[137,8],[118,6],[90,4],[60,4],[37,7],[20,10]];
        s.water(rim,[close(rim)],89);
        s.trees([[4,11,18],[13,13,10],[131,16,11],[140,16,15],[152,13,23]]);
    }],
    ['alpine-pass','High pass switchbacks','Alpine woodcuts',s=>{
        s.sky(119,63,5,'mist');
        s.ridge([[0,30],[12,43],[27,40],[43,61],[51,53],[68,68],[84,49],[99,53],[113,37],[132,48],[143,42],[156,50]],'snow');
        s.hill([[0,25],[23,31],[45,23],[61,29],[82,24],[99,29],[123,22],[156,31]],'bare');
        s.river([[87,24,.3],[77,20,1],[79,17,1.2],[93,14,1.6],[93,12,1.9],[69,9,2.3],[59,6,2.8],[65,3,3.4],[78,-2,4.8]],81);
        s.rocks([[19,10],[27,16],[35,13],[38,8],[31,5],[22,6]]);
        s.pine(142,8,20);s.pine(151,10,14);
    }],
    ['mesa-ladder','Terraces of the red mesa','Desert woodcuts',s=>{
        s.sky(34,55,9,'mist');
        s.ridge([[0,29],[18,31],[23,40],[39,40],[45,31],[63,30],[72,39],[75,50],[86,51],[88,56],[111,55],[114,45],[122,42],[126,31],[145,30],[156,33]],'rock');
        s.hill([[0,21],[18,17],[42,20],[59,14],[81,18],[112,12],[140,17],[156,19]],'dune');
        s.cactus(30,9,1.24);s.cactus(17,13,.46);
        s.rocks([[110,6],[117,11],[126,10],[130,5],[123,2],[112,2]]);
    }],
    ['dune-tide','Night tide of the dunes','Desert woodcuts',s=>{
        s.sky(110,60,5,'clear');
        s.ridge([[0,27],[22,32],[33,29],[59,39],[73,30],[89,34],[111,27],[127,32],[143,28],[156,30]],'mist');
        s.hill([[0,37],[21,34],[50,22],[81,32],[98,42],[123,31],[156,24]],'dune');
        s.hill([[0,13],[27,18],[54,12],[87,16],[119,27],[141,26],[156,20]],'dune');
        s.reeds(25,9,.65);s.reeds(32,8,.4);
    }],
    ['salt-lagoon','White salt lagoon','Desert woodcuts',s=>{
        s.sky(48,56,8,'wind');
        s.ridge([[0,34],[18,39],[32,34],[46,37],[64,28],[80,36],[92,32],[107,46],[122,39],[138,42],[156,32]],'rock');
        s.hill([[0,22],[28,17],[60,21],[85,16],[111,20],[139,23],[156,27]],'bare');
        const salt=[[9,12],[25,15],[43,14],[69,17],[79,15],[99,16],[119,14],[135,10],[119,9],[91,11],[71,8],[48,9],[34,7],[19,8]];
        s.water(salt,[close(salt)],49);
        s.rocks([[127,4],[135,8],[143,7],[150,4],[145,1],[130,1]]);
        s.cactus(11,4,.59);
    }],
    ['oxbow','Oxbow from the ridge','River woodcuts',s=>{
        s.sky(121,54,7,'mist');
        s.ridge([[0,35],[17,41],[31,36],[44,40],[67,29],[83,35],[104,33],[123,38],[138,34],[156,40]],'mist');
        s.hill([[0,32],[18,30],[40,33],[62,23],[76,24],[94,26],[125,32],[156,27]]);
        s.river([[73,23,.7],[67,20,1.3],[52,18,1.7],[42,15,2.1],[48,12,2.7],[72,11,3.6],[98,10,3.7],[114,9,4.4],[120,7,4.8],[109,4,5.8],[73,-3,8]],86);
        s.trees([[8,16,14],[17,17,11],[33,19,7],[139,15,11],[149,13,18]]);
        s.reeds(21,4,.8);s.reeds(28,4,.48);
    }],
    ['willow-backwater','Reed beds at dusk','River woodcuts',s=>{
        s.sky(66,51,9,'mist');
        s.hill([[0,32],[21,35],[39,31],[57,34],[77,29],[94,33],[117,31],[140,34],[156,31]],'bare');
        s.sea(24,66);s.boat(106,16,.81);
        s.hill([[0,12],[21,9],[34,11],[45,7],[63,3],[80,-3],[122,-3],[141,3],[156,7]],'grass');
        for(const [x,y,z] of [[3,4,.9],[11,5,1.28],[20,4,.87],[34,5,.72],[44,4,.46],[145,2,.62],[151,4,.92]])s.reeds(x,y,z);
    }],
    ['mill-reach','Hillside hamlet before the rain','River woodcuts',s=>{
        s.sky(117,58,5,'wind');
        s.ridge([[0,42],[22,48],[39,40],[61,43],[76,34],[94,41],[115,35],[133,40],[156,36]],'rock');
        s.hill([[0,28],[17,31],[37,28],[57,23],[78,26],[101,26],[126,23],[156,26]],'grass');
        s.river([[68,23,.7],[61,21,1],[57,18,1.7],[66,14,2.5],[82,11,3.5],[99,8,5],[107,4,6],[100,-3,8]],76);
        s.cabin(23,27,1.0);s.cabin(43,25,.55);
        s.trees([[3,20,17],[14,21,15],[133,16,12],[142,16,18],[152,16,15]]);
    }]
];
function CNCStockPrints(register,add){
    plates.forEach(([slug,name,category,compose],i)=>{
        const id='stock-prints-'+slug;
        register(id,()=>{const scene=scenery(87+i*137);compose(scene);add(id,name,category,scene.finish());});
    });
}
if(typeof module!=='undefined'&&module.exports)module.exports=CNCStockPrints;
else root.CNCStockPrints=CNCStockPrints;
})(globalThis);
