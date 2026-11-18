/* KerfKite original gift blanks. Outer contours and piercings are separate paths. */
(function (root) {
'use strict';
const pi=Math.PI, tau=2*pi;
const indices=n=>Array.from({length:n},(_,i)=>i);
const close=p=>[...p,[...p[0]]];
function ellipse(x,y,rx,ry,n=32){
    return close(indices(n).map(i=>[x+rx*Math.cos(tau*i/n),y+ry*Math.sin(tau*i/n)]));
}
function transform(path,sx,sy,dx=0,dy=0){
    return path.map(([x,y])=>[x*sx+dx,y*sy+dy]);
}
function linePath(start, commands) {
    const path=[start];
    for(const cmd of commands){
        if(cmd.length===2)path.push(cmd);
        else {
            const [x,y]=path.at(-1),[a,b,c,d,e,f]=cmd;
            for(let i=1;i<=10;i++){
                const t=i/10,u=1-t;
                path.push([u*u*u*x+3*u*u*t*a+3*u*t*t*c+t*t*t*e,
                    u*u*u*y+3*u*u*t*b+3*u*t*t*d+t*t*t*f]);
            }
        }
    }
    return path;
}
function symmetric(start,commands){
    const half=linePath(start,commands);
    return [...half,...half.slice(0,-1).reverse().map(([x,y])=>[-x,y])];
}
function star(r,inner,n,stretch=1) {
    return close(indices(2*n).map(i=>{
        const a=pi/2+pi*i/n,s=i%2?inner:r;
        return [s*Math.cos(a),s*Math.sin(a)*stretch];
    }));
}
// Individually traced silhouettes. The clear top of each accepts a single jump ring.
const drops=[
    ['Ginkgo fan',30,()=>symmetric([0,34],[[6,34,6,29,6,26],[9,20,18,18,24,13],[29,8,28,1,23,-2],[21,0,18,0,16,-3],[12,-7,8,-5,6,-4],[4,-5,2,-6,0,-7]])],
    ['Rain lantern',29,()=>symmetric([0,34],[[5,34,6,31,7,26],[9,19,20,8,20,-1],[20,-12,10,-18,0,-20]])],
    ['Paper kite',28,()=>symmetric([0,35],[[6,30],[19,11],[0,-23]])],
    ['Fluted shell',30,()=>symmetric([0,34],[[5,34,6,32,6,27],[11,23,25,10,25,0],[25,-5,22,-7,18,-6],[16,-13,12,-14,8,-10],[7,-17,3,-18,0,-18]])],
    ['Tapered seed',26,()=>symmetric([0,33],[[15,25,20,11,15,-2],[13,-10,6,-17,0,-22]])],
    ['Pagoda lantern',30,()=>symmetric([0,35],[[6,34],[7,29],[8,26],[19,21],[16,18],[16,2],[10,-3],[8,-11],[3,-13],[0,-19]])],
    ['Bullion tassel',30,()=>symmetric([0,34],[[6,34,7,30,7,27],[7,24],[12,22],[12,16],[9,14],[16,-9],[11,-8],[10,-14],[6,-11],[3,-18],[0,-13]])],
    ['Scallop arch',26,()=>symmetric([0,32],[[14,32,20,20,20,11],[20,-5],[17,-4,16,-9,13,-9],[10,-7,9,-12,6,-12],[3,-10,2,-15,0,-15]])],
    ['Spade finial',28,()=>symmetric([0,34],[[7,32,7,28,7,24],[10,19,20,15,20,6],[20,-2,11,-4,7,0],[6,-7,11,-14,14,-16],[0,-16]])],
    ['Paddle reed',28,()=>symmetric([0,33],[[8,33,10,26,9,21],[7,16,7,14,9,10],[16,3,18,-8,9,-15],[6,-18,3,-20,0,-21]])],
    ['Pomegranate',30,()=>symmetric([0,34],[[4,31],[9,34],[7,25],[15,23,23,16,23,6],[24,-7,14,-17,0,-17]])],
    ['Bellflower',30,()=>symmetric([0,35],[[5,35,8,30,8,25],[10,17,11,7,19,-1],[21,-4,22,-6,24,-8],[18,-8,15,-9,12,-13],[8,-11,5,-11,0,-18]])]
];
function diamond(x,y,rx,ry){return close([[x-rx,y],[x,y+ry],[x+rx,y],[x,y-ry]]);}
function cutAccent(i,x,y,size){
    const type=i%6;
    if(type===0)return ellipse(x,y,size,size*.78);
    if(type===1)return diamond(x,y,size*.82,size*1.2);
    if(type===2)return transform(star(size,size*.46,4),1,1,x,y);
    if(type===3)return transform(symmetric([0,1],[[.6,.45,.9,0,.8,-.38],[.74,-.82,.42,-1,0,-1]]),size,size,x,y);
    if(type===4)return transform(star(size,size*.64,6),1,1,x,y);
    return transform(symmetric([0,1],[[.33,.61,.8,.3,.8,-.19],[.8,-.63,.5,-.98,0,-1]]),size*.85,size*1.25,x,y);
}
const badges=[
    ['Tailored ticket', [[31,-16],[34,-13],[34,-7],[31,-5,31,1,34,3],[34,12],[30,16],[0,16]]],
    ['Apothecary', [[27,-16],[27,-12],[32,-12],[32,-8],[36,-8],[36,8],[32,8],[32,12],[27,12],[27,16],[0,16]]],
    ['Gallery arch',[[32,-16],[32,1],[32,12,21,18,0,18]]],
    ['Scalloped linen',[[25,-16],[28,-16,29,-14,29,-12],[34,-14,36,-10,34,-6],[39,-5,39,4,34,6],[37,10,34,15,29,13],[29,17,26,17,24,17],[0,17]]],
    ['Shield',[[20,-13],[29,-8],[33,1],[33,13],[18,13],[9,15],[0,18]]],
    ['Clipped octagon',[[22,-16],[35,-5],[35,5],[22,16],[0,16]]],
    ['Scrolled tablet',[[24,-16],[26,-12,30,-11,33,-10],[28,-5,28,5,33,10],[29,11,26,12,24,16],[0,16]]],
    ['Pearl oval',[[21,-16,35,-9,35,0],[35,9,21,16,0,16]]],
    ['Saddle plaque',[[24,-16],[28,-12,33,-12,36,-12],[36,12],[33,12,28,12,24,16],[0,16]]],
    ['Ribbon swallowtail',[[36,-16],[29,0],[36,16],[0,16]]],
    ['Keystone',[[26,-16],[32,8],[25,9],[21,15],[8,15],[5,19],[0,19]]],
    ['Ogee tablet',[[20,-16],[23,-14,24,-11,26,-10],[34,-8,37,-4,37,0],[37,4,34,8,26,10],[24,11,23,14,20,16],[0,16]]]
];
function label(i){return symmetric([0,-16],badges[i][1]);}
function labelOnStem(i,stem,width,point){
    const outline=label(i);
    // The ellipse and shield have curved lower edges; intersect at the shank width.
    const right=outline.findIndex(([x])=>x>=width),u=outline[right-1],v=outline[right];
    const y=u[1]+(v[1]-u[1])*(width-u[0])/(v[0]-u[0]);
    const edge=outline.slice(right,outline.length-right);
    const tail=point?[[-width,-stem+8],[0,-stem],[width,-stem+8]]:[[-width,-stem],[width,-stem]];
    return [[width,y],...edge,[-width,y],...tail,[width,y]];
}
function earrings(i){
    const [,hole,draw]=drops[i],outline=draw();
    const centre=[6,6,8,7,7,10,17,8,8,4,6,10][i];
    return [outline,ellipse(0,hole,1.55,1.55),cutAccent(i,0,centre,i===6?3.6:5.1)];
}
function pendants(i){
    const rim=label(i),x=[-15,-18,-15,-16,-15,-16,-16,-18,-16,-14,-16,-18][i];
    return [transform(rim,.83,1),ellipse(x,5,1.7,1.7),ellipse(-x,5,1.7,1.7),cutAccent(i,0,-3,5.6)];
}
function tags(i){
    const outline=label(i),paths=[outline,ellipse(-23,0,1.9,1.9)];
    // Tiny etched edge marks designate the tied end and leave the face free.
    paths.push(...[-1,1].map(side=>linePath([-17,side*8],[[-15,side*7,-14,side*5,-15,side*3]])));
    return paths;
}
function foodPicks(i){
    return [labelOnStem(i,51,2.4,true),...[-1,1].map(side=>diamond(side*19,0,1.6,1.6))];
}
function bookmark(i){
    // The name of each marker follows its top finial. Stalk is one continuous cut.
    const crown=transform(drops[i][2](),.48,.46,0,9);
    const turn=Math.floor((crown.length-1)/2),right=crown.slice(0,turn+1);
    // Keep the upper outline down to the widest point, then join the page blade.
    const widest=right.reduce((best,p,k)=>p[0]>right[best][0]?k:best,0);
    const upper=right.slice(0,widest+1);
    const half=[...upper,[6.1,upper.at(-1)[1]-5],[6.1,-52],[4.7,-55],[0,-57]];
    return [[...half,...half.slice(0,-1).reverse().map(([x,y])=>[-x,y])],
        ellipse(0,8+.46*drops[i][1],1.20,1.20),diamond(0,-47,1.6,3)];
}
function bookplate(i){
    const outer=label(i);
    return [outer,transform(outer,.86,.75),...[-1,1].map(x=>ellipse(x*22.5,0,1.0,1.0))];
}
function coaster(i){
    const outer=[
        ()=>ellipse(0,0,30,30),()=>diamond(0,0,33,33),
        ()=>close([[-22,-26],[22,-26],[28,-20],[28,20],[22,26],[-22,26],[-28,20],[-28,-20]]),
        ()=>close(indices(6).map(k=>[30*Math.cos(k*pi/3),30*Math.sin(k*pi/3)])),
        ()=>symmetric([0,29],[[13,28,17,24,20,18],[30,18,33,7,27,0],[33,-7,30,-18,20,-18],[15,-25,6,-29,0,-29]]),
        ()=>close(indices(96).map(k=>{const a=k*tau/96,r=28+2*Math.cos(8*a);return [r*Math.cos(a),r*Math.sin(a)];})),
        ()=>transform(label(7),.92,1.62),()=>transform(label(3),.83,1.68),
        ()=>transform(label(4),.84,1.50),()=>transform(label(10),.9,1.6),
        ()=>symmetric([0,30],[[18,30,28,20,28,9],[28,-23],[22,-29],[0,-29]]),
        ()=>close(indices(12).map(k=>[30*Math.cos(k*pi/6+pi/12),30*Math.sin(k*pi/6+pi/12)]))
    ][i]();
    const paths=[outer,transform(outer,.86,.86)];
    for(let j=0;j<4;j++){
        const a=(2*j+1)*pi/4,r=i===1?13.8:18;
        paths.push(cutAccent(i,r*Math.cos(a),r*Math.sin(a),1.5));
    }
    return paths;
}
function handlePlate(i) {
    const p=transform(label(i),1.2,.74),offset=[25,27,25,28,24,24,27,26,27,24,25,27][i];
    return [p,ellipse(-offset,0,1.8,1.8),ellipse(offset,0,1.8,1.8),
        ...[-1,1].map(s=>linePath([s*17,-5.1],[[s*15,-5.1,s*14,-6.7,s*12,-6.7],[s*6,-6.7]]))];
}
function tasselFob(i) {
    const outline=transform(drops[i][2](),.80,.83);
    // An oblong slot admits narrow ribbon; its width stays inside each crown.
    const cy=.83*drops[i][1]-2;
    const slot=transform(symmetric([0,1.05],[[1.2,1.05],[2.6,1.05,2.6,-1.05,1.2,-1.05],[0,-1.05]]),1,1,0,cy);
    const featureY=[5,5,7,4,3,7,15,2,6,3,5,6][i];
    return [outline,slot,...[-1,1].map(s=>ellipse(s*3,featureY,1.02,1.02))];
}
function silhouetteCut(i) {
    // Single outer contour; the waste aperture lifts as a single piece.
    const profiles=[
        ()=>symmetric([0,15],[[2,8],[9,4],[5,0],[8,-9],[0,-4]]),
        ()=>symmetric([0,11],[[2,15,8,16,11,12],[17,7,11,0,0,-11]]),
        ()=>symmetric([0,15],[[12,8,12,0,5,-6],[4,-9,3,-12,0,-15]]),
        ()=>symmetric([0,12],[[4,9,5,6,6,4],[11,8,16,7,16,3],[16,-4,10,-7,3,-7],[2,-11],[0,-13]]),
        ()=>star(15,7,6),()=>symmetric([0,16],[[5,12],[5,6],[10,8],[9,2],[13,3],[12,-2],[16,-1],[14,-6],[3,-8],[1,-13],[0,-13]]),
        ()=>symmetric([0,14],[[6,16,10,12,8,7],[14,8,16,2,12,-2],[5,-6,4,-9,0,-15]]),
        ()=>symmetric([0,15],[[3,9],[9,12],[8,6],[14,7],[12,0],[9,-4],[9,-7],[3,-7],[2,-14],[0,-14]]),
        ()=>symmetric([0,13],[[10,13,14,8,14,4],[13,1,11,-1,10,-4],[7,1,4,1,3,-3],[2,-6,4,-8,0,-14]]),
        ()=>symmetric([0,16],[[3,9],[10,9],[6,4],[10,-1],[5,-1],[10,-9],[2,-8],[2,-13],[0,-13]]),
        ()=>star(14,9.3,8,.88),
        ()=>symmetric([0,12],[[4,11,5,6,4,3],[7,8,13,9,15,5],[16,1,12,-2,7,-3],[12,-4,13,-8,10,-11],[4,-12,1,-8,0,-4]])
    ];
    return [profiles[i]()];
}
const cutNames=['Comet glint','Folded heart','Elm seed','Floral plume','Lantern star','Fern sprig',
    'Cloud finial','Maple sprig','Scalloped bell','Fir tree','Eight-point sun','Four-wing moth'];
const purposes=[
    ['earring','Pierced earring blanks',i=>drops[i][0]+' earring',earrings],
    ['pendant','Two-point pendants',i=>badges[i][0]+' collar pendant',pendants],
    ['gift-tag','Writing gift tags',i=>badges[i][0]+' parcel tag',tags],
    ['place-pick','Table display picks',i=>badges[i][0]+' display pick',foodPicks],
    ['bookmark','Finial bookmarks',i=>drops[i][0]+' bookmark',bookmark],
    ['bookplate','Personal library plates',i=>badges[i][0]+' library plate',bookplate],
    ['coaster','Personalisation coasters',i=>['Round','Kite','Clipped square','Hexagon','Cloud','Eight-lobe','Oval','Scalloped','Shield','Keystone','Arch','Twelve-sided'][i]+' monogram coaster',coaster],
    ['plate','Drawer & hamper labels',i=>badges[i][0]+' hamper plate',handlePlate],
    ['fob','Ribbon-slot bag charms',i=>drops[i][0]+' ribbon charm',tasselFob],
    ['aperture','Gift box apertures',i=>cutNames[i]+' box window',silhouetteCut]
];
function CNCStockGifts(register,add){
    for(const [key,category,name,draw] of purposes){
        for(const i of indices(12)){
            const id='stock-gifts-'+key+'-'+i;
            register(id,()=>add(id,name(i),category,draw(i)));
        }
    }
}
if(typeof module!=='undefined'&&module.exports)module.exports=CNCStockGifts;
else root.CNCStockGifts=CNCStockGifts;
})(globalThis);
