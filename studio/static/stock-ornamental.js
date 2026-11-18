/* Original ornamental drawings for KerfKite. Coordinates are Cartesian, Y up. */
(function (root) {
'use strict';

const pi = Math.PI;
const tau = pi * 2;
const range = n => Array.from({length:n}, (_, i) => i);
const close = points => [...points, points[0]];
const sample = (count, f) => range(count + 1).map(i => f(i / count));
function arc(x, y, rx, ry, a, b, n = 24) {
    return sample(n, t => [x + rx * Math.cos(a + (b-a)*t), y + ry * Math.sin(a + (b-a)*t)]);
}
function oval(x, y, rx, ry) {
    return close(arc(x,y,rx,ry,0,tau,32).slice(0,-1));
}
function box(left, bottom, right, top) {
    return close([[left,bottom],[right,bottom],[right,top],[left,top]]);
}
function bez(a,b,c,d,n=12) {
    return sample(n,t => {
        const s=1-t;
        return [0,1].map(k=>s*s*s*a[k]+3*s*s*t*b[k]+3*s*t*t*c[k]+t*t*t*d[k]);
    });
}
// Draw each seed between endpoints. Parallel edges form a single closed cut path.
function seed(x,y,length,width,angle=0) {
    const upper=bez([0,0],[length*.27,width],[length*.73,width],[length,0],9);
    const lower=bez([length,0],[length*.73,-width],[length*.27,-width],[0,0],9);
    return place([...upper,...lower.slice(1)],x,y,angle);
}
function place(path,x,y,angle=0,sx=1,sy=sx) {
    const c=Math.cos(angle),s=Math.sin(angle);
    return path.map(([u,v])=>[x+u*sx*c-v*sy*s, y+u*sx*s+v*sy*c]);
}
function diamond(x,y,rx,ry) {
    return close([[x-rx,y],[x,y+ry],[x+rx,y],[x,y-ry]]);
}
function star(x,y,n,outer,inner) {
    return close(range(n*2).map(i=>{
        const a=pi/2+i*pi/n,r=i%2?inner:outer;
        return [x+Math.cos(a)*r,y+Math.sin(a)*r];
    }));
}
function interpolate(a,b,t){return a.map((v,i)=>v+(b[i]-v)*t);}
function parallelRings(rx,ry,count,inset) {
    return range(count).map(i=>oval(0,0,rx-i*inset,ry-i*inset));
}
function mark(style,x,y,r) {
    if(style===0) return [diamond(x,y,r,r*.92)];
    if(style===1) return [star(x,y,4,r,r*.30)];
    if(style===2) return [oval(x,y,r,r),oval(x,y,r*.43,r*.43)];
    return range(4).map(i=>seed(x+Math.cos(i*pi/2)*r*.16,y+Math.sin(i*pi/2)*r*.16,r*.82,r*.20,i*pi/2));
}

// Stonework openings use a common spring line; the centreline is left clear.
function lancet(x,y,w,h) {
    const l=[x-w/2,y],r=[x+w/2,y],apex=[x,y+h],spring=y+h*.58;
    return [l,[x-w/2,spring],...bez([x-w/2,spring],[x-w/2,spring+h*.18],[x-w*.16,y+h*.90],apex).slice(1),
        ...bez(apex,[x+w*.16,y+h*.90],[x+w/2,spring+h*.18],[x+w/2,spring]).slice(1),r,l];
}
function tracery(style,bay) {
    const n=[2,3,4][bay], rise=[93,102,113][bay],w=78;
    const paths=[lancet(0,0,w,rise),lancet(0,3,w-6,rise-8)];
    const span=66/n, lowerTop=rise*.49;
    for(let i=0;i<n;i++){
        const x=(i+.5)*span-33;
        paths.push(lancet(x,7,span-3,lowerTop-7));
        if(style===1||style===3) paths.push(seed(x,14,lowerTop-22,span*.15,pi/2));
        if(style===2) paths.push(diamond(x,lowerTop*.54,span*.25,lowerTop*.22));
    }
    const ry=rise*.69,rad=rise*.12;
    paths.push(oval(0,ry,rad,rad),...mark(style,0,ry,rad*.77));
    return paths;
}
function fanlight(style,bay) {
    const rays=[5,7,9][bay],r=50,h=[30,42,49][bay];
    const paths=[close(arc(0,0,r,h,0,pi,48)),close(arc(0,2,r-3,h-4,0,pi,48))];
    const at=(u,a)=>[u*46*Math.cos(a),2+u*(h-5)*Math.sin(a)];
    const inner=.23;
    paths.push(arc(0,2,46*inner,(h-5)*inner,0,pi,28));
    for(let i=1;i<rays;i++) paths.push([at(inner,i*pi/rays),at(1,i*pi/rays)]);
    for(let i=0;i<rays;i++){
        const a=i*pi/rays,b=(i+1)*pi/rays,m=(a+b)/2;
        if(style===0) paths.push([at(.70,a),at(.90,m),at(.70,b)]);
        if(style===1) paths.push(close([at(.35,m),at(.63,a+.18*(b-a)),at(.90,m),at(.63,b-.18*(b-a))]));
        if(style===2) paths.push([...bez(at(.37,m),at(.54,a),at(.77,a),at(.93,m)),...bez(at(.93,m),at(.77,b),at(.54,b),at(.37,m)).slice(1)]);
        if(style===3) paths.push(arc(0,2,46*.78,(h-5)*.78,a,b,9),[at(.78,m),at(1,m)]);
    }
    return paths;
}
function shoji(style,bay) {
    const cols=bay+2,rows=[3,4,3][bay],w=cols*19,h=rows*18;
    const paths=[box(0,0,w+6,h+6),box(3,3,w+3,h+3)];
    for(let c=1;c<cols;c++)paths.push([[3+c*19,3],[3+c*19,h+3]]);
    for(let r=1;r<rows;r++)paths.push([[3,3+r*18],[w+3,3+r*18]]);
    for(let c=0;c<cols;c++)for(let r=0;r<rows;r++){
        const x=12.5+c*19,y=12+r*18;
        if(style===0) paths.push(diamond(x,y,9.5,9), ...((c+r)%2?[]:[diamond(x,y,4.1,3.9)]));
        if(style===1) {const v=(c+r)%2?1:-1;paths.push([[x-9.5,y-v*9],[x+9.5,y+v*9]],[ [x-9.5,y],[x,y+v*9] ],[[x,y-v*9],[x+9.5,y]]);}
        if(style===2){
            if((c+r)%2===0)paths.push(...range(4).map(i=>place(seed(0,0,10.7,2.4),x,y,pi/4+i*pi/2)));
            else paths.push(diamond(x,y,7.2,6.8));
        }
        if(style===3){if((c+r)%2===0){paths.push(box(x-5.5,y-5.5,x+5.5,y+5.5),diamond(x,y,3.6,3.6));}
            else paths.push([[x-9.5,y],[x+9.5,y]],[[x,y-9],[x,y+9]]);}
    }
    return paths;
}
function compass(style,bay) {
    const tips=[6,8,12][bay],paths=parallelRings(48,48,2,2);
    const polar=(r,a)=>[r*Math.cos(a),r*Math.sin(a)];
    const hub=7;paths.push(oval(0,0,hub,hub));
    for(let i=0;i<tips;i++){
        const a=pi/2+tau*i/tips,half=pi/tips,outer=(i%2===0||style===2)?42:35;
        const left=polar(15,a+.80*half),right=polar(15,a-.80*half),tip=polar(outer,a);
        paths.push(close([polar(hub+1,a),left,tip,right]),[polar(hub+1,a),tip]);
        if(style===1) paths.push(seed(...polar(23,a+half),12,1.6,a+half));
        if(style===2) paths.push(close([polar(19,a+half),polar(26,a+half*.76),polar(36,a+half),polar(26,a+half*1.24)]));
        if(style===3) paths.push([polar(25,a+half*.70),polar(34,a+half),polar(25,a+half*1.30)]);
    }
    return paths;
}
// Patchwork lines meet on shared seams. Borders are generated once per block.
function quilt(style,bay) {
    const n=bay+2,s=18,paths=[box(-2,-2,n*s+2,n*s+2),box(0,0,n*s,n*s)];
    for(let i=1;i<n;i++)paths.push([[s*i,0],[s*i,n*s]],[[0,s*i],[n*s,s*i]]);
    for(let x=0;x<n;x++)for(let y=0;y<n;y++){
        const cx=(x+.5)*s,cy=(y+.5)*s,turn=pi/2*((x<n/2?1:0)+2*(y<n/2?1:0));
        let lines;
        if(style===0) lines=[[[ -9,-9],[9,9]],[[0,0],[9,-9]],[[0,-9],[4.5,-4.5]]];
        if(style===1) lines=[diamond(0,0,9,9),diamond(0,0,4.5,4.5),[[-9,-9],[-4.5,-4.5]],[[9,9],[4.5,4.5]]];
        if(style===2) lines=[arc(-9,-9,18,18,0,pi/2,18),arc(-9,-9,12,12,0,pi/2,14),arc(-9,-9,6,6,0,pi/2,10)];
        if(style===3) lines=[[[ -9,-3],[3,-3],[3,9]],[[ -9,3],[-3,3],[-3,9]],[[9,-9],[3,-3]]];
        paths.push(...lines.map(p=>place(p,cx,cy,turn)));
    }
    return paths;
}
// Ground wires stop at each element's clear space.
function moulding(style,bay) {
    const n=[5,7,9][bay],pitch=14,w=n*pitch;
    const paths=[box(0,-10,w,10),[[0,7.8],[w,7.8]],[[0,-7.8],[w,-7.8]]];
    for(let i=0;i<n;i++){
        const x=pitch*(i+.5);
        if(style===0) paths.push([...bez([x-4,4],[x-6,-1],[x-2,-5],[x,-5.4]),...bez([x,-5.4],[x+2,-5],[x+6,-1],[x+4,4]).slice(1),[x-4,4]],
            [[x-6,4],[x-6,-1],[x-5.4,-2]]);
        if(style===1) paths.push(seed(x-5.6,0,11.2,5.8),oval(x,0,1.5,1.5));
        if(style===2) paths.push(...mark((i+bay)%2?0:1,x,0,5.6));
        if(style===3) paths.push([[x-7,-5],[x+5,-5],[x+5,5],[x-4,5],[x-4,-1],[x+1,-1],[x+1,2]]);
    }
    return paths;
}
function balcony(style,bay) {
    const n=bay+3,w=n*18,paths=[box(0,0,w+6,36),box(3,3,w+3,33)];
    for(let i=1;i<n;i++)paths.push([[3+18*i,3],[3+18*i,33]]);
    for(let i=0;i<n;i++){
        const x=12+i*18;
        if(style===0){paths.push(seed(x,4,28,7,pi/2),oval(x,18,2,2));}
        if(style===1){paths.push(diamond(x,18,8,14),diamond(x,18,4.4,8));}
        if(style===2){
            const curl=sample(36,t=>{const a=-pi/2+t*tau*1.20,r=5.8*(1-t)+.6;return [r*Math.cos(a),r*Math.sin(a)];});
            const lower=place(curl,x,10);const upper=place(curl,x,26,pi);
            paths.push(lower,upper,bez(lower[0],[x-15,5],[x+15,31],upper[0]));
        }
        if(style===3){paths.push(oval(x,18,7,7),[[x,3],[x,11]],[[x,25],[x,33]],[[x-5.8,4],[x,9],[x+5.8,4]],[[x-5.8,32],[x,27],[x+5.8,32]]);}
    }
    return paths;
}
function guilloche(style,bay) {
    const n=[6,9,12][bay],rx=54,ry=38;
    const paths=parallelRings(rx,ry,2,2.2);
    // Nested rails use the same phase: the engraver never retraces a crossing.
    for(let band=0;band<3;band++){
        paths.push(close(sample(n*10,t=>{
            const a=t*tau, w=style===0?Math.cos(n*a):style===1?Math.abs(Math.sin(n*a/2))*2-1:
                style===2?.65*Math.cos(n*a)+.35*Math.cos(2*n*a):Math.cos(n*a+.55*Math.sin(n*a));
            const r=.70+.046*band+.09*w;
            return [rx*r*Math.cos(a),ry*r*Math.sin(a)];
        }).slice(0,-1)));
    }
    paths.push(oval(0,0,rx*.51,ry*.51),oval(0,0,rx*.45,ry*.45));
    for(let i=0;i<n;i++){
        const a=(i+.5)*tau/n;
        paths.push(oval(rx*.56*Math.cos(a),ry*.56*Math.sin(a),.85,.85));
    }
    return paths;
}
function marquetry(style,bay) {
    const n=bay+5,w=n*16,paths=[box(0,0,w,17),[[0,2],[w,2]],[[0,15],[w,15]]];
    for(let i=0;i<n;i++){
        const x=16*i;
        if(style===0){paths.push([[x,2],[x+8,8.5],[x,15]],[[x+5,2],[x+13,8.5],[x+5,15]]);}
        if(style===1){paths.push(diamond(x+8,8.5,8,6.5),diamond(x+8,8.5,3.5,3));}
        if(style===2){
            // Parallel stringers turn together; adjacent bays meet at the same height.
            const steps=[[0,4],[5.3,4],[5.3,7],[10.7,7],[10.7,10],[16,10]];
            for(const offset of [0,2.5])paths.push(steps.map(([u,v])=>[u+x,(i%2?14-v:v)+offset]));
        }
        if(style===3){paths.push([[x,8.5],[x+8,15],[x+16,8.5],[x+8,2],[x,8.5]],seed(x+3,8.5,10,3.8));}
    }
    return paths;
}
function wavePanel(style,bay) {
    const count=[3,4,5][bay],w=count*20,h=33,paths=[box(-2,-2,w+2,h+2),box(0,0,w,h)];
    if(style<2){
        for(let row=0;row<3;row++)for(let c=0;c<count;c++){
            const x=10+c*20,y=1+row*10;
            for(const r of (style===0?[9.4,6.4,3.4]:[9.4,4.8])) paths.push(arc(x,y,r,r,0,pi,16));
            if(style===1) paths.push(diamond(x,y+1.6,1,1.2));
        }
    } else {
        for(let row=0;row<5;row++){
            const base=4+row*6;
            if(style===2)paths.push(sample(count*18,t=>[t*w,base+1.9*Math.sin(t*count*tau+row*.45)]));
            else paths.push(range(count*4+1).map(i=>[5*i,base+[0,2,0,-2][i%4]]));
        }
    }
    return paths;
}
function ceiling(style,bay) {
    const n=[6,8,10][bay],paths=parallelRings(46,46,2,2.4);
    paths.push(oval(0,0,8,8),...mark(style,0,0,5));
    for(let i=0;i<n;i++){
        const a=tau*i/n,r=10,w=48/n;
        let petal;
        if(style===0)petal=[seed(r,0,29,w*1.6),seed(r+6,0,15,w*.55)];
        if(style===1)petal=[close([[r,0],[23,-w],[37,-w*.32],[41,0],[37,w*.32],[23,w]]),[[18,0],[35,0]]];
        if(style===2)petal=[...range(2).map(j=>seed(16,j?1:-1,22,w*.58,j?.17:-.17)),diamond(12,0,2,2)];
        if(style===3)petal=[seed(r,0,19,w*1.3),diamond(36,0,5.2,w*.64)];
        paths.push(...petal.map(p=>place(p,0,0,a)));
    }
    return paths;
}
function deco(style,bay) {
    const n=[5,7,9][bay],w=54,h=54,paths=[];
    // The silhouette doubles as the outer contour; avoid adding a rectangle behind it.
    paths.push(close([[-27,0],[27,0],[27,22],[22,22],[22,31],[17,31],[17,40],[11,40],[11,47],[0,h],[-11,47],[-11,40],[-17,40],[-17,31],[-22,31],[-22,22],[-27,22]]));
    const spring=[0,4];
    for(let i=0;i<n;i++){
        const x=(i/(n-1)-.5)*(w-8),height=44-Math.abs(x)*1.17;
        if(style===0)paths.push([[x*.28,5],[x,height]]);
        if(style===1)paths.push(seed(x*.26,5,Math.hypot(x*.74,height-5),1.25,Math.atan2(height-5,x*.74)));
        if(style===2)paths.push([[x*.42,5],[x*.42,12],[x,height-3],[x,height]]);
        if(style===3)paths.push(bez([x*.35,spring[1]],[x*.24,16],[x,18],[x,height]));
    }
    paths.push([[-24,3],[-15,3] ],[[15,3],[24,3]]);
    return paths;
}
function tile(style,bay) {
    const n=[2,3,4][bay],d=22,w=n*d,paths=[box(-2,-2,w+2,w+2),box(0,0,w,w)];
    for(let row=0;row<n;row++)for(let col=0;col<n;col++){
        const x=col*d+d/2,y=row*d+d/2;
        if(style===0){paths.push(star(x,y,8,9.5,6.2),diamond(x,y,3,3));}
        if(style===1){paths.push(close(range(8).map(i=>[x+10*Math.cos(pi/8+i*pi/4),y+10*Math.sin(pi/8+i*pi/4)])),star(x,y,4,6.5,2.1));}
        if(style===2){const v=(row+col)%2===0?1:-1;paths.push(...range(4).map(i=>place(seed(1.5,0,8,2.7),x,y,pi/4*v+i*pi/2)),oval(x,y,1,1));}
        if(style===3){paths.push(diamond(x,y,9.5,9.5),...range(4).map(i=>place(diamond(4.4,0,3.4,1.6),x,y,i*pi/2)));}
    }
    return paths;
}
function stitch(style,bay) {
    const n=[4,5,6][bay],paths=[];
    function running(points){
        for(let i=1;i<points.length;i++){
            const a=points[i-1],b=points[i],num=Math.max(1,Math.round(Math.hypot(a[0]-b[0],a[1]-b[1])/3.8));
            for(let j=0;j<num;j++)paths.push([interpolate(a,b,(j+.14)/num),interpolate(a,b,(j+.86)/num)]);
        }
    }
    for(let row=0;row<3;row++)for(let c=0;c<n;c++){
        const x=12*c,y=12*row;
        if(style===0)running(diamond(x+6,y+6,5.8,5.8));
        if(style===1){running([[x+1,y+3],[x+6,y+8],[x+11,y+3]]);running([[x+1,y+6],[x+6,y+11],[x+11,y+6]]);}
        if(style===2){const a=[[0,0],[4,0],[4,4],[8,4],[8,8],[12,8]];running(a.map(([u,v])=>[x+u,y+((row+c)%2?11-v:v+1)]));}
        if(style===3){running([[x+1,y+6],[x+11,y+6]]);running([[x+6,y+1],[x+6,y+4.8]]);running([[x+6,y+7.2],[x+6,y+11]]);}
    }
    return paths;
}
function corner(style,bay) {
    const span=[36,46,58][bay], count=[3,4,5][bay],paths=[];
    paths.push([[0,span],[0,0],[span,0]],[[3,span-4],[3,3],[span-4,3]]);
    const elbow=13;
    for(let i=0;i<count;i++){
        const u=(i+.5)/count;
        const x=elbow+(span-elbow-5)*u,y=5.6,angle=.88;
        const length=7*(1-u)+4;
        let element;
        if(style===0)element=[seed(0,0,length,1.65,angle)];
        if(style===1)element=[place(diamond(length/2,0,length/2,1.4),0,0,angle)];
        if(style===2)element=[bez([0,0],[0,length*.5],[length*.6,length*.8],[length*.8,length*.5]),
            oval(length*.75,length*.20,.65,.65)];
        if(style===3)element=[[[0,0],[0,length*.68],[length*.35,length*.68],[length*.35,length],[length*.60,length]]];
        for(const p of element){
            paths.push(place(p,x,y),p.map(([px,py])=>[y+py,x+px]));
        }
    }
    paths.push(...mark(style,7.3,7.3,2.25));
    return paths;
}
function cameo(style,bay) {
    const n=[8,12,16][bay],rx=[44,39,32][bay],ry=[30,35,41][bay];
    const paths=[oval(0,0,rx*.81,ry*.81),oval(0,0,rx*.75,ry*.75)];
    const at=(r,a)=>[rx*r*Math.cos(a),ry*r*Math.sin(a)];
    // A continuous shaped rim follows the decoration around the blank inscription field.
    const boundary=close(sample(n*12,t=>{
        const a=t*tau, swell=(1+Math.cos(n*a))/2;
        return at(1+.105*(style===2?swell*swell:swell),a);
    }).slice(0,-1));
    paths.push(boundary);
    for(let i=0;i<n;i++){
        const a=tau*i/n, [x,y]=at(.94,a),size=Math.min(rx,ry)*.069;
        if(style===0)paths.push(oval(x,y,size,size));
        if(style===1)paths.push(place(diamond(0,0,size*1.55,size),x,y,a));
        if(style===2)paths.push(place(seed(-size*1.65,0,size*3.3,size*.80),x,y,a+pi/2));
        if(style===3)paths.push(star(x,y,4,size*1.40,size*.57));
    }
    return paths;
}
function divider(style,bay) {
    const span=[34,45,56][bay],count=[2,3,4][bay],paths=[];
    paths.push(...mark(style,0,0,5));
    for(const side of [-1,1]){
        const items=[];
        items.push(bez([7,0],[span*.47,-1.8],[span*.68,1.1],[span,0],20));
        for(let i=0;i<count;i++){
            const x=9+i*(span-12)/count, len=9.2-(i/count)*4;
            if(style===0){items.push(seed(x,1.4,len,1.65,.82),seed(x,-1.4,len,1.65,-.82));}
            if(style===1){items.push([[x,2.3],[x+len*.55,7.4],[x+len*.90,2.3]],[[x,-2.3],[x+len*.55,-7.4],[x+len*.90,-2.3]]);}
            if(style===2){const flare=bez([x,1.8],[x+2,6.3],[x+len*.8,7],[x+len,4.4]);items.push(flare,flare.map(([u,v])=>[u,-v]));}
            if(style===3){items.push(oval(x+len*.35,4.4,1.25,1.25),oval(x+len*.35,-4.4,1.25,1.25));}
        }
        paths.push(...items.map(p=>p.map(([x,y])=>[x*side,y])));
    }
    return paths;
}

// Each motif is drafted in three structural layouts, such as a pair of lancets,
// a triplet or a four-light opening. Recipes materialise a single design on demand.
const series = [
    ['lancet','Stone tracery',['Lozenge oculus','Lily lancets','Wheel oculus','Quatrefoil lantern'],['paired lights','triplet','four lights'],tracery],
    ['fanlight','Transom fanlights',['Chevron spokes','Lozenge panes','Tulip panes','Split rim'],['segment arch','elliptical transom','half-round transom'],fanlight],
    ['screen','Lattice screens',['Diamond screen','Diagonal reeds','Leaf lattice','Square lantern'],['narrow screen','hall panel','wide screen'],shoji],
    ['compass','Compass marquetry',['Split spear','Leaf bearings','Tessellated compass','Chevron bearings'],['six points','eight points','twelve points'],compass],
    ['quilt','Patchwork blocks',['Turning pinwheel','Nested diamond','Quarter-circle paths','Cabin stair'],['four patch','nine patch','sixteen patch'],quilt],
    ['moulding','Architectural friezes',['Egg and dart','Reed and bead','Star and lozenge','Stepped key'],['five-bay strip','seven-bay strip','nine-bay strip'],moulding],
    ['balcony','Balcony ironwork',['Almond spindle','Double lozenge','Counter-scroll grille','Oculus spindle'],['three bays','four bays','five bays'],balcony],
    ['guilloche','Engine-turned engraving',['Fluted engine turn','Scalloped engine turn','Double-frequency engine turn','Pinched engine turn'],['six lobes','nine lobes','twelve lobes'],guilloche],
    ['inlay','Veneer inlay bands',['Feather chevron','Diamond stringing','Parquet stair','Shuttle and eye'],['short run','medium run','long run'],marquetry],
    ['wave','Wave engraving panels',['Three-ridge tide','Pearled breakers','Swell lines','Folded current'],['three repeats','four repeats','five repeats'],wavePanel],
    ['ceiling','Plaster ceiling roses',['Fluted leaf rose','Faceted acanthus rose','Twin-leaf rose','Bead-tip rose'],['six rays','eight rays','ten rays'],ceiling],
    ['deco','Art Deco escutcheons',['Sunrise stepped crest','Reed stepped crest','Lightning stepped crest','Fountain stepped crest'],['five flutes','seven flutes','nine flutes'],deco],
    ['tile','Geometric tilework',['Eight-point mosaic','Octagon mosaic','Windflower tile','Petal diamond tile'],['four-tile inset','nine-tile panel','sixteen-tile field'],tile],
    ['stitch','Running stitch samplers',['Lozenge running stitch','Double chevron stitch','Stepped path stitch','Broken cross stitch'],['four columns','five columns','six columns'],stitch],
    ['corner','Bookbinding corner tools',['Laurel corner','Lance corner','Quill corner','Stair corner'],['small folio','quarto','large folio'],corner],
    ['cameo','Blank cameo surrounds',['Pearled cameo','Lozenge cameo','Seed-bezel cameo','Star-bezel cameo'],['oval label','round seal','upright portrait'],cameo],
    ['divider','Printer’s rules',['Leaf rule','Crown rule','Plume rule','Pearl rule'],['short measure','half measure','long measure'],divider]
];

function CNCStockOrnamental(register,add) {
    for(const [key,category,motifs,layouts,draw] of series){
        motifs.forEach((motif,style)=>layouts.forEach((layout,bay)=>{
            const id='stock-ornamental-'+key+'-'+style+'-'+bay;
            register(id,()=>add(id,motif+' · '+layout,category,draw(style,bay)));
        }));
    }
}
if(typeof module!=='undefined'&&module.exports) module.exports=CNCStockOrnamental;
else root.CNCStockOrnamental=CNCStockOrnamental;
})(globalThis);
