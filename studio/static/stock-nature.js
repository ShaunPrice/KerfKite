/* Original natural-history drawings. The masters use page coordinates, converted
 * to the editor's Y-up plane at registration. All curves flatten to polylines. */
(function (root) {
'use strict';

const TAU = Math.PI * 2;
const lerp = (a, b, t) => a.map((v, i) => v * (1 - t) + b[i] * t);
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
const sample = (n, fn) => Array.from({length: n + 1}, (_, i) => fn(i / n));
function ring(points) { return [...points, points[0].slice()]; }
function oval(x, y, rx, ry, angle = 0) {
    return ring(sample(40, t => {
        const u = rx * Math.cos(t * TAU), v = ry * Math.sin(t * TAU);
        return [x + u * Math.cos(angle) - v * Math.sin(angle), y + u * Math.sin(angle) + v * Math.cos(angle)];
    }).slice(0, -1));
}

// The small path vocabulary below is used only by these hand-drawn masters.
function trace(description) {
    const paths = [], tokens = description.match(/[MLQCZ]|-?\d*\.?\d+/g);
    let i = 0, path, mode;
    const point = () => [Number(tokens[i++]), Number(tokens[i++])];
    while (i < tokens.length) {
        if (/^[MLQCZ]$/.test(tokens[i])) mode = tokens[i++];
        if (mode === 'M') {
            path = [point()]; paths.push(path); mode = 'L';
        } else if (mode === 'Z') {
            if (dist(path[0], path[path.length - 1]) > 1e-8) path.push(path[0].slice());
            mode = null;
        } else if (mode === 'L') {
            path.push(point());
        } else {
            const a = path[path.length - 1], b = point(), c = point(), d = mode === 'C' ? point() : null;
            const steps = Math.max(3, Math.ceil((dist(a,b) + dist(b,c) + (d ? dist(c,d) : 0)) / 3.4));
            for (let j = 1; j <= steps; j++) {
                const t = j / steps, ab = lerp(a,b,t), bc = lerp(b,c,t);
                path.push(d ? lerp(lerp(ab,bc,t), lerp(bc,lerp(c,d,t),t), t) : lerp(ab,bc,t));
            }
        }
    }
    return paths;
}

const sea = [
    ['Manta ray', 'M50 20 Q47 20 43 14 Q41 18 43 24 C31 26 14 28 3 17 C8 35 19 51 38 54 Q42 58 47 56 Q49 82 60 94 Q52 73 52 55 Q60 59 64 52 C82 48 93 32 98 16 C84 27 70 26 57 24 Q59 18 56 13 Q53 19 50 20 Z M10 24 Q25 40 41 40 M91 23 Q77 39 60 40 M43 30 Q47 27 49 31 M52 31 Q54 27 58 30'],
    ['Humpback sounding', 'M5 48 Q12 42 24 40 C40 28 63 22 78 38 Q82 41 88 35 Q94 22 99 26 Q98 36 93 42 Q98 44 98 53 Q89 52 85 47 L79 47 Q69 45 61 48 Q54 54 45 55 C39 64 30 70 23 69 Q30 59 34 55 Q17 57 7 53 Q3 51 5 48 Z M8 49 Q18 52 31 48 M11 52 Q18 54 28 52 M35 46 Q40 44 44 45 M59 32 Q65 30 69 33', [[28,45,1,1]]],
    ['Moon jelly', 'M7 44 C10 10 35 5 51 6 C77 5 96 21 97 44 Q92 40 86 46 Q80 41 74 47 Q68 41 61 47 Q55 42 49 47 Q43 41 36 46 Q29 40 24 45 Q18 40 13 45 Q10 41 7 44 Z M14 35 Q22 17 37 14 M61 12 Q80 15 89 32 M19 48 C12 62 25 65 17 78 M32 49 C25 67 38 73 28 87 M43 51 C36 61 50 71 43 91 M55 51 C60 63 47 76 58 87 M68 51 C62 68 76 74 69 83 M82 49 C77 63 89 69 82 79', [[45,27,4,3],[56,27,4,3],[46,34,4,3],[56,34,4,3]]],
    ['Ocean sunfish', 'M10 49 Q13 32 33 27 L41 8 Q44 3 46 6 L50 25 Q67 26 76 39 L84 42 82 48 85 54 81 59 81 64 74 66 Q63 73 54 72 L50 91 Q47 98 44 92 L38 72 Q17 68 13 58 L8 56 12 53 8 51 Z M30 37 Q37 47 31 58 M38 49 Q46 43 52 48 Q47 53 38 54 Z M70 38 Q77 51 71 62 M42 66 Q50 68 58 65', [[21,44,2.5,2.5]]],
    ['Loggerhead turtle', 'M33 29 Q34 21 40 19 Q37 10 43 7 Q50 3 54 10 L55 17 Q61 16 65 22 C72 16 82 12 91 15 Q81 26 70 32 Q77 49 69 64 Q80 69 81 77 Q72 77 62 71 Q50 82 38 74 Q34 77 29 77 L31 71 Q22 70 19 75 Q16 61 27 57 Q22 45 27 37 C16 43 9 52 5 52 Q8 34 33 29 Z M34 30 Q45 19 57 23 Q69 27 68 45 Q67 63 53 70 Q36 77 30 60 Q23 45 34 30 Z M39 30 L47 25 56 30 55 39 45 42 37 37 Z M36 40 L44 45 43 54 36 58 31 50 Z M48 44 L58 41 64 45 61 54 52 57 46 53 Z M45 57 L52 60 52 66 44 69 38 62 Z'],
    ['Flying fish', 'M5 47 Q17 38 36 40 C35 31 31 16 22 9 Q46 13 59 38 Q69 39 77 45 L94 27 91 47 99 63 83 56 76 53 Q59 58 45 57 Q50 69 50 79 Q37 70 31 57 Q15 57 5 49 Z M40 37 Q35 24 29 17 M47 36 L35 19 M54 37 L42 23 M21 43 Q26 48 22 53 M39 61 L47 73 M59 47 Q65 48 71 47', [[14,46,1.8,1.8]]],
    ['Seahorse', 'M55 16 L57 7 51 11 46 6 43 13 37 11 38 17 Q28 18 28 26 L14 31 17 37 33 33 Q40 37 45 31 Q46 38 37 45 Q25 56 35 67 C39 75 55 76 56 82 C56 87 49 87 48 83 C44 77 39 81 42 88 C47 98 64 95 65 83 Q68 72 57 65 Q54 60 61 53 L68 50 65 45 70 40 64 38 68 32 61 32 Q63 22 55 16 Z M35 47 Q45 47 53 54 M33 54 Q42 55 51 61 M34 61 Q42 63 52 66 M42 70 Q53 72 59 77 M49 22 Q54 21 56 27', [[37,23,2,2]]],
    ['Spotted boxfish', 'M7 45 L19 31 Q32 24 56 28 L64 17 68 19 66 30 Q73 33 76 41 L94 31 Q91 44 96 56 L76 49 Q70 58 63 61 L64 70 56 64 Q27 69 15 56 L6 53 10 49 Z M24 36 Q30 43 25 52 M33 48 Q39 40 48 43 Q45 51 36 54 Z', [[17,42,2,2],[35,33,2,2],[47,34,2.5,2.5],[60,37,2,2],[54,48,2,2],[43,58,2,2],[64,51,2,2]]],
];
const shorebirds = [
    ['Pied oystercatcher','M7 43 L26 36 Q27 27 35 26 Q44 26 45 35 Q49 39 60 39 L84 36 74 46 89 45 76 54 Q64 59 52 57 Q39 57 34 47 L29 42 7 45 Z M45 39 Q54 46 75 40 Q65 53 50 51 Q45 47 45 39 Z M46 58 L44 72 35 74 M45 72 L49 74 M57 59 L57 73 49 77 M57 73 L63 74', [[34,32,1.4,1.4]]],
    ['Long-billed curlew','M7 50 Q10 33 28 29 Q34 20 41 25 Q45 28 45 34 Q50 36 60 37 L81 33 75 40 85 38 Q81 47 69 52 Q56 59 43 52 Q35 47 33 37 L28 34 Q14 37 7 50 Z M44 37 Q57 42 73 37 Q62 50 49 49 Z M46 56 L48 69 43 80 36 81 M43 80 L46 82 M59 57 L59 71 55 82 49 83 M55 82 L60 84', [[36,28,1,1]]],
    ['Crowned heron','M5 28 L25 22 Q28 14 36 16 L54 12 41 18 55 16 40 22 Q45 27 39 33 L34 40 Q42 35 53 37 Q65 36 79 45 L90 48 78 49 86 55 Q68 61 51 54 Q34 55 27 47 Q22 42 30 32 L33 26 27 26 5 29 Z M43 41 Q57 35 76 46 Q67 48 58 45 M50 47 Q59 52 72 51 M46 56 L43 68 48 83 39 86 M48 83 L55 86 M58 58 L56 72 62 84 54 88 M62 84 L68 85', [[31,21,1,1]]],
    ['Crested puffin','M13 37 Q16 28 26 24 Q25 16 37 13 Q49 10 55 23 Q53 30 59 37 L74 52 79 68 72 66 74 73 Q61 75 51 70 Q29 70 24 54 Q21 45 27 36 Q19 39 13 37 Z M17 34 Q21 27 25 28 L24 34 Z M31 29 Q29 21 39 19 Q48 18 49 25 Q45 32 37 30 M52 37 Q63 41 71 61 Q57 59 51 46 Z M39 70 L38 78 29 82 44 81 47 79 M53 72 L54 80 47 84 63 82', [[38,24,1.5,1.5]]],
    ['Flamingo at rest','M22 27 Q26 18 34 19 Q46 19 44 31 Q43 37 38 43 Q33 49 40 49 Q52 40 63 42 L90 47 82 51 87 54 Q68 64 49 60 Q31 60 29 52 Q27 45 34 36 Q40 29 34 26 Q31 25 30 29 L25 34 18 31 17 36 Q12 30 16 25 Z M47 51 Q61 45 78 49 Q68 57 53 57 M47 62 L44 76 53 89 45 91 M58 64 L66 75 51 75 M23 26 L20 30', [[28,23,1,1]]],
    ['Kingfisher on watch','M8 32 L33 27 Q31 18 43 16 L39 12 50 15 48 11 58 17 Q64 24 58 33 Q68 43 73 54 L85 64 74 62 81 71 67 67 Q52 69 40 58 Q31 48 34 34 L9 34 Z M46 34 Q54 29 58 39 Q67 47 68 58 Q54 54 47 45 Z M49 39 L62 50 M44 60 L44 68 39 70 M52 65 L54 70 49 72 M33 73 Q56 66 75 74', [[40,24,2,2]]],
    ['Pelican with folded pouch','M7 41 L35 28 Q34 17 44 17 Q56 18 52 30 Q48 40 53 41 Q61 36 71 42 L89 49 83 51 93 55 Q70 65 53 59 Q40 57 38 44 L37 37 Q25 52 7 47 Q4 45 7 41 Z M10 42 L35 32 M11 46 Q21 47 30 40 M53 46 Q66 40 80 49 Q68 56 56 54 Z M55 61 L54 71 44 74 60 74 M67 62 L68 72 60 76 75 75', [[42,23,1.5,1.5]]],
    ['Tern in flight','M5 51 Q18 47 34 45 Q37 40 43 42 Q48 45 52 42 C47 31 42 16 23 7 Q55 13 67 36 L93 22 Q84 35 72 44 L94 46 79 51 91 57 71 54 Q60 57 53 55 C53 64 49 73 44 81 Q45 64 38 54 L28 50 5 52 Z M32 13 Q51 23 58 37 M42 20 Q55 28 61 36 M53 48 Q61 45 68 46', [[40,45,1,1]]],
];

const seeds = [
    ['Acorn and cup','M23 41 Q20 28 39 26 Q52 22 63 27 Q74 31 72 42 Q48 49 23 41 Z M25 44 C27 61 33 73 49 79 L52 84 55 78 Q69 68 71 45 M49 23 Q47 17 54 12 L57 14 Q52 18 53 24 M33 40 Q33 33 39 30 M42 43 Q43 35 48 29 M53 43 Q55 35 59 31 M64 41 L67 35 M31 50 Q33 65 43 72'],
    ['Twin maple keys','M45 49 C32 44 13 38 8 17 Q7 11 12 10 Q34 13 46 38 C50 42 54 38 58 32 Q67 14 84 7 Q89 7 87 16 C83 32 71 46 57 51 Q54 62 47 60 Q40 58 45 49 Z M15 16 Q30 25 42 42 M14 21 Q28 34 39 40 M63 41 Q76 26 82 15 M66 34 Q71 23 79 16', [[49,52,2.5,3]]],
    ['Poppy capsule','M33 24 L27 19 40 20 43 14 49 19 55 13 59 20 70 18 64 24 Q58 28 62 33 C73 47 69 61 53 66 L52 86 49 87 48 66 C30 64 26 48 36 34 Q41 28 33 24 Z M39 26 Q47 31 59 25 M40 35 Q32 52 42 60 M49 34 Q45 48 48 62 M56 35 Q63 48 56 60'],
    ['Winged ash seed','M45 66 C48 51 31 25 32 6 Q45 8 51 23 C61 45 53 57 52 67 Q55 76 51 86 Q47 93 43 86 Q40 78 45 66 Z M36 13 Q42 28 49 38 Q52 50 49 61 M40 18 Q50 28 53 44 M47 71 Q44 80 47 85'],
    ['Opening milkweed pod','M23 66 Q19 46 32 26 Q45 10 65 7 Q69 22 58 45 Q49 63 30 70 Q26 76 20 77 L17 74 23 66 Z M29 63 Q36 32 60 15 Q53 43 29 63 Z M32 54 Q37 50 40 44 M36 45 L47 31 M42 39 L51 26 M23 59 L25 48 M30 33 L35 26 M47 56 L55 45'],
    ['Horse chestnut shell','M16 49 L12 42 19 42 16 33 24 35 23 26 30 29 32 19 38 23 43 16 47 23 56 17 57 23 65 19 65 27 74 25 72 32 81 31 77 38 84 41 79 45 86 52 76 53 79 62 71 61 70 69 62 66 60 73 52 69 46 75 42 69 34 72 33 66 24 67 25 60 17 61 20 54 Z M24 52 C29 31 53 23 69 34 Q56 38 42 56 Q33 63 24 52 Z M32 49 Q36 39 49 35 M46 59 Q57 45 69 40'],
    ['Lotus seed head','M15 35 C19 18 63 12 78 25 Q90 37 70 47 C48 58 21 53 15 39 Z M19 45 Q31 66 46 67 L55 86 59 84 51 65 Q70 59 76 46', [[28,35,3,2],[39,27,3,2],[52,24,3,2],[66,27,3,2],[45,37,4,2.5],[60,35,3,2],[35,44,3,2],[53,44,3,2]]],
    ['Pea pod dehiscence','M9 65 Q14 61 17 53 C26 27 63 37 81 23 Q86 19 91 21 Q88 24 84 25 Q78 40 61 48 Q38 61 18 61 Q13 69 9 65 Z M19 56 Q33 38 65 35 Q70 34 75 31 M27 56 Q33 57 36 52 M39 52 Q46 53 49 47 M52 47 Q58 47 62 41 M65 41 Q71 40 74 34'],
];

function crystalCluster(crystals) {
    const outlines = [], visible = [];
    const clip = typeof module !== 'undefined' ? require('./ornaments.js').clip : root.CNCOrnaments.clip;
    // The order runs from the front of the specimen towards the back.
    for (const [x,y,w,h,lean,tip] of crystals) {
        const tr = ([a,b]) => [x + a + lean*b, y - b];
        const ridgeHeight = h - Math.min(w * 1.25, h * .24);
        const ridgeX = w * tip;
        const contour = ring([[-w/2,3],[-w/2,ridgeHeight],[ridgeX,h],[w/2,ridgeHeight+w*.17],
            [w/2,w*.15],[w*.14,0]].map(tr));
        const faces = [contour, [[ridgeX,h],[w*.11,ridgeHeight-w*.34],[w*.14,0]].map(tr),
            [[-w/2,ridgeHeight],[w*.11,ridgeHeight-w*.34],[w/2,ridgeHeight+w*.17]].map(tr)];
        let exposed = faces;
        for (const mask of outlines) exposed = clip(exposed,mask);
        visible.push(...exposed); outlines.push(contour);
    }
    return visible;
}
const minerals = [
    ['Cathedral quartz',[[50,86,25,74,.01,-.12],[32,82,20,41,-.2,0],[70,82,17,52,.22,.15],[17,77,12,28,-.4,0]]],
    ['Twin scepter',[[41,81,28,57,-.13,-.17],[63,83,26,76,.08,.16]]],
    ['Needle spray',[[51,85,11,45,-.1,0],[39,80,9,65,-.32,-.1],[61,82,10,70,.24,0],[67,80,8,43,.48,.1],[31,78,8,42,-.52,0],[48,77,8,70,-.05,0]]],
    ['Smoky citadel',[[49,82,31,51,-.24,-.09],[69,82,24,61,.15,-.3],[30,78,23,31,-.38,.14],[58,77,20,63,-.19,.2]]],
    ['Alpine crystal fan',[[45,85,18,46,-.25,-.13],[57,84,15,44,.3,.18],[69,80,14,38,.58,.2],[33,80,15,39,-.69,-.2],[51,77,18,68,.11,.05]]],
    ['Herkimer pair',[[37,65,37,40,.24,.27],[69,68,27,38,.4,-.23]]],
    ['Tourmaline organ pipes',[[38,82,16,48,.02,-.14],[51,85,15,65,.05,.1],[65,83,13,54,.08,.12],[27,79,12,33,.01,.1],[74,78,10,38,.12,.2]]],
    ['Geode spires',[[38,79,18,24,-.4,-.2],[49,83,14,31,.04,.14],[63,78,18,28,.45,.12],[30,76,17,37,-.55,-.2],[72,74,17,42,.49,.2],[52,74,24,54,.08,.19]]],
    ['Inclined monolith',[[41,83,31,70,.29,-.25],[67,82,16,26,.48,.15],[24,78,15,23,-.29,-.1]]],
    ['Satin spar bundle',[[34,80,10,42,.3,-.14],[44,82,10,54,.3,-.14],[54,82,10,61,.3,-.14],[65,79,9,50,.3,-.14],[23,78,11,31,.3,-.14]]],
];

function plume(length, width, bend, tip, base, rag, notches, bars, fringe) {
    const spine = t => [46 + bend*t*t + 3*Math.sin(Math.PI*t), 91 - length*t];
    const edge = (t,side) => {
        const [x,y] = spine(t);
        const vane = width * Math.sin(Math.PI*t)**base * (1 + tip*(t-.5));
        const cuts = notches.reduce((n,[u,depth,s]) => s === side
            ? n + depth*Math.max(0,1-Math.abs(u-t)/.035) : n, 0);
        return [x+side*vane*(1-cuts)*(side<0?.89:1),y+rag*Math.sin(t*Math.PI)*side];
    };
    const outline = [...sample(65,t=>edge(t,-1)),...sample(65,t=>edge(1-t,1)).slice(1,-1)];
    const paths = [ring(outline), sample(32,t=>spine(t*.93)), [spine(0),[44.6,99]]];
    for (let j=0;j<fringe;j++) {
        const t=.08+j*(.73/Math.max(1,fringe-1));
        for (const side of [-1,1]) {
            const e = edge(t+.075,side);
            paths.push(sample(5,k=>lerp(lerp(spine(t),[spine(t)[0]+side*width*.22,spine(t)[1]-1],k),
                lerp([spine(t)[0]+side*width*.22,spine(t)[1]-1],e,k),k)));
        }
    }
    for (const t of bars) {
        for (const side of [-1,1]) {
            const to = edge(t+.075, side), from = spine(t);
            paths.push(sample(8,k=>[from[0]+(to[0]-from[0])*k,from[1]+(to[1]-from[1])*k+1.6*Math.sin(Math.PI*k)]));
        }
    }
    return paths;
}
const feathers = [
    ['Primary flight quill',[85,11,23,-1,.82,1.8,[[.65,.27,1],[.36,.35,-1]],[],9]],
    ['Rounded coverlet',[65,20,-3,.85,.56,2,[[.4,.25,-1]],[],6]],
    ['Notched tail vane',[82,14,0,-.65,.65,2.5,[[.48,.59,1],[.68,.55,-1],[.29,.37,-1]],[],8]],
    ['Short barred feather',[62,18,9,.63,.62,2,[],[.105,.252,.398,.544,.69],6]],
    ['Wind-bent plume',[87,15,-30,-.6,.72,3,[[.5,.28,1],[.25,.35,-1]],[],8]],
    ['Tapered hackle',[77,10,-7,-1.1,.95,3,[],[],10]],
    ['Spotted owl vane',[68,22,4,.55,.6,2,[[.34,.26,-1],[.65,.32,1]],[.13,.28,.43,.58],6]],
    ['Split pennant feather',[83,16,17,-.56,.68,2,[[.77,.7,-1],[.51,.65,1],[.36,.55,-1]],[],7]],
];

function snow(arms, spacing, branches, cap, center) {
    const paths = [], projection = (i,rad,side) => {
        const a = i*TAU/arms;
        return [50+rad*Math.cos(a)-side*Math.sin(a),50+rad*Math.sin(a)+side*Math.cos(a)];
    };
    paths.push(ring(Array.from({length:arms},(_,i)=>projection(i,center,0))));
    for (let i=0;i<arms;i++) {
        const project=(r,s=0)=>projection(i,r,s);
        paths.push([project(center),project(46)]);
        branches.forEach(([start,len,fork])=>{
            for(const s of [-1,1]) {
                paths.push([project(start), project(start+len*.73,s*len)]);
                if(fork) {
                    const t=.59;
                    paths.push([project(start+len*.73*t,s*len*t),project(start+len*.73*t+spacing,s*len*t)]);
                }
            }
        });
        if(cap) paths.push([project(46-cap),project(46-cap*.5,cap*.32),project(46),project(46-cap*.5,-cap*.32),project(46-cap)]);
    }
    return paths;
}
const frost = [
    ['Dendrite rosette',6,4,[[12,9,1],[22,12,1],[33,8,0]],0,5],
    ['Fernlike stellar crystal',6,3,[[12,9,1],[19,11,1],[26,10,1],[33,7,1],[39,4,0]],0,6],
    ['Hexagonal spear crystal',6,3,[[16,6,0],[26,8,0],[32,4,0]],9,10],
    ['Twelve-rayed frost star',12,2,[[20,4,0],[30,6,0],[37,4,0]],4,10],
    ['Openwork frost wheel',6,4,[[19,12,1],[35,6,0]],6,16],
    ['Ice spire',6,3,[[11,6,0],[17,6,0],[23,6,0],[29,6,0],[35,6,0]],5,5],
    ['Needle-branched plate',6,4,[[11,11,0],[24,15,0],[34,7,0]],0,4],
    ['Twelve-point rime',12,2,[[24,5,1],[34,5,0]],7,13],
    ['Window frost floret',6,3,[[14,6,1],[21,10,1],[32,7,1]],7,8],
    ['Crown-tip crystal',6,2,[[20,9,1],[27,8,1],[34,7,1]],6,12],
];

function fanShell(ribs, span, depth, shoulder, tilt, lip, bands) {
    const hinge=[50,82], angle=span*Math.PI/360;
    function rim(t) {
        const a=(2*t-1)*angle;
        const r=depth*(1-.10*(a/angle)**2)*(1+lip*(.5-.5*Math.cos(TAU*ribs*t)));
        return [50+r*Math.sin(a)*shoulder+tilt*t*t,82-r*Math.cos(a)];
    }
    const fan=sample(ribs*6,rim);
    const outline=ring([...fan,[57+tilt*.25,78],[59,85],[53,84],[50,81],[45,84],[40,84],[43,78]]);
    const paths=[outline];
    for(let k=1;k<ribs;k++){
        const t=k/ribs;
        paths.push(sample(15,u=>{
            const p=lerp(hinge,rim(t),.11+.89*u);
            return [p[0]+2.2*Math.sin(Math.PI*u)*(2*t-1),p[1]];
        }));
    }
    for(const radius of bands) {
        // Keep each accent between ribs. Endpoints stop short of the ridge.
        for(let k=0;k<ribs;k++)
            paths.push(sample(4,u=>lerp(hinge,rim((k+.18+u*.62)/ribs),radius)));
    }
    return paths;
}
const valves = [
    ['Pilgrim scallop',11,107,63,1.0,0,.034,[.83]],
    ['Fine-ribbed cockle',17,125,51,.90,1,.018,[]],
    ['Lion paw scallop',8,128,63,1.04,-3,.077,[.76,.87]],
    ['Fan mussel',7,62,72,.79,11,.019,[.48,.64,.8]],
    ['Moon scallop',13,145,55,1.06,0,.024,[.88]],
    ['Winged clam',7,110,51,1.24,13,.035,[.78]],
    ['Fluted heart cockle',10,84,67,.92,-3,.065,[.83,.91]],
    ['Rayed bay scallop',9,119,59,.98,4,.043,[]],
];

function ammonite(turns, rate, ovality, tilt, divisions, ornate) {
    const total=TAU*turns;
    function spiral(a,shift=0) {
        const r=40*Math.exp((a-total)*rate), phase=a+tilt+shift;
        return [50+ovality*r*Math.cos(phase),50+r*Math.sin(phase)];
    }
    const paths=[sample(Math.ceil(total*14),t=>spiral(t*total))];
    paths.push(sample(12,t=>lerp(spiral(total),spiral(total-TAU),t)));
    for(let j=0;j<divisions;j++) {
        const a=total*(.36+.62*j/divisions);
        paths.push(sample(9,t=>{
            const sag=.14*Math.sin(t*Math.PI)+(ornate?.06*Math.sin(t*TAU*2):0);
            return lerp(spiral(a-TAU,sag),spiral(a,sag),t);
        }));
    }
    return paths;
}
const coils = [
    ['Nautilus chamber section',2.0,.19,1.02,-.62,23,false],
    ['Ribbed ramshorn fossil',2.75,.145,.94,1.33,41,false],
    ['Frilled ammonite suture',2.35,.18,1.07,.6,28,true],
    ['Fast-whorl goniatite',1.7,.28,.92,-1.3,17,true],
    ['Planispiral limestone cast',2.65,.15,1.23,.3,30,false],
    ['Fine striated ammonoid',2.2,.20,.82,-.48,43,false],
    ['Broad-chamber coil',2.15,.175,1.04,1.1,14,false],
    ['Ripple-sutured fossil',2.5,.16,1.17,-.83,35,true],
];

function seaStar(arms, waist, twist, slender, beads) {
    const sector=TAU/arms, band=sector*waist;
    function rotate(rad,side,a){return [50+rad*Math.cos(a)-side*Math.sin(a),50+rad*Math.sin(a)+side*Math.cos(a)];}
    const rim=[],paths=[oval(50,50,waist*18,waist*18)];
    for(let i=0;i<arms;i++) {
        const a=i*sector-Math.PI/2;
        // A single contour passes successively through each tip and its adjacent valleys.
        for(let k=0;k<32;k++){
            const t=(k/32-.5)*sector, u=Math.abs(t)/(sector*.5);
            const length=slender+(44-slender)*Math.pow(1-u,1.85);
            rim.push(rotate(length,0,a+t+twist*(1-u)**2));
        }
        paths.push(sample(15,t=>{
            const r=slender*.28+t*(38-slender*.28);
            return rotate(r,0,a+twist*(.05+.88*t)**2);
        }));
        for (const side of [-1,1]) {
            paths.push(sample(9,t=>rotate(slender*.39+slender*.54*t, side*(2.9+1.4*Math.sin(Math.PI*t)),a)));
        }
        for(let j=0;j<beads;j++) {
            const t=j/Math.max(1,beads-1),r=slender+(.3+.44*t)*(39-slender);
            for(const s of [-1,1]){
                const [x,y]=rotate(r,(2.35-1.2*t)*s*(band/.12),a+twist*((r-slender)/(44-slender))**2);
                paths.push(oval(x,y,.7-.2*t,.7-.2*t));
            }
        }
    }
    return [ring(rim),...paths];
}
const stars = [
    ['Cushion star',5,.08,.03,29,3], ['Slender bat star',5,.08,-.045,19,2],
    ['Six-arm reef star',6,.08,.055,21,2], ['Ochre sea star',5,.095,.12,18,4],
    ['Sun star',10,.085,.08,21,1], ['Seven-ray sand star',7,.075,-.06,18,2],
    ['Long-arm biscuit star',5,.075,.21,15,2], ['Nine-point crown star',9,.07,-.09,23,1],
];

function sandDollar(lobes, tall, channels, petals, crown, edge) {
    const boundary = t => {
        const a=t*TAU, scallop=1-edge*(.5+.5*Math.cos(lobes*a))**3;
        return [50+39*scallop*Math.cos(a),50+39*tall*scallop*Math.sin(a)];
    };
    const paths=[ring(sample(100,boundary).slice(0,-1))], hub=[50,50-crown];
    for(let i=0;i<5;i++) {
        const angle=-Math.PI/2+i*TAU/5, L=petals[i]*tall;
        const point=(r,s)=>[hub[0]+r*Math.cos(angle)-s*Math.sin(angle),hub[1]+r*Math.sin(angle)+s*Math.cos(angle)];
        const petal=side=>sample(17,t=>point(3+L*t,side*(.38+L*.14*Math.sin(Math.PI*t)**.8)));
        paths.push(ring([...petal(-1),...petal(1).reverse()]));
        for(let j=1;j<=6;j++){
            const t=j/7, w=L*.12*Math.sin(Math.PI*t)**.8;
            paths.push([point(3+L*t,-w),point(3+L*t+.9,-.4)]);
            paths.push([point(3+L*t+.9,.4),point(3+L*t,w)]);
        }
        if(channels.includes(i)){
            const [x,y]=point(3+L+4,0);
            paths.push(oval(x,y,2.8,1,angle));
        }
    }
    return paths;
}
const tests = [
    ['Five-keyhole sand dollar',5,.94,[0,1,2,3,4],[19,18,18,18,18],0,.04],
    ['Arrowhead sand dollar',3,1.09,[2,3],[18,18,22,22,18],3,.08],
    ['Round sea biscuit',5,1.03,[],[17,18,18,18,18],0,.018],
    ['Elongated pansy shell',5,1.20,[1,2,3,4],[17,16,17,17,16],4,.065],
    ['Three-slot sand dollar',3,.88,[0,2,3],[18,22,16,16,22],0,.04],
    ['Wide petal sea biscuit',5,.86,[],[23,24,21,21,24],1,.026],
    ['Notched keyhole dollar',5,1.04,[0,2,3],[18,23,18,18,23],2,.105],
    ['Short-petal cake urchin',5,.98,[],[12,13,14,14,13],-2,.018],
];

function agate(spec) {
    const [stretch, faceting, bump, phase, channel, offsets] = spec;
    const points=(size, density) => ring(sample(104,t=>{
        const a=t*TAU;
        const facet=1-faceting*Math.abs(Math.cos(3*a+phase));
        const r=size*facet*(1+.085*Math.cos(3*a-phase)+bump*Math.cos(5*a+phase)+density*Math.sin(11*a));
        // A drifting core gives the growth lines a quiet side and a thick side.
        return [50+channel*(1-size)+38*r*stretch*Math.cos(a),50+7*(1-size)+40*r*Math.sin(a)];
    }).slice(0,-1));
    return offsets.map((d,i)=>points(d,i===0?.012:.004));
}
const agates = [
    ['Fortification agate',[1.08,.18,.032,.4,5,[1,.91,.87,.72,.64,.6,.43,.24]]],
    ['Blue-lace geode',[1.34,.03,.08,.6,11,[1,.93,.85,.80,.72,.65,.42]]],
    ['Thunder egg section',[.83,.11,.05,1.52,9,[1,.89,.71,.67,.45,.39,.20]]],
    ['Lagoon agate',[1.17,.02,.045,2.4,-9,[1,.94,.79,.73,.68,.41]]],
    ['Banded jasper cobble',[.98,.20,.045,-.75,4,[1,.86,.81,.64,.53,.34,.29]]],
    ['Hollow geode rim',[1.16,.09,.07,-1.1,2,[1,.9,.85,.79,.72]]],
    ['Eye agate pebble',[1.45,.04,.029,.1,17,[1,.93,.77,.51,.40,.32,.20,.12]]],
    ['Shadow agate nodule',[.94,.13,.085,2.92,-8,[1,.9,.83,.6,.44,.37,.30,.18]]],
];

function frond(pairs, lean, width, sweep, toothed, skew) {
    const stem=t=>[50+lean*t*t,91-77*t];
    const ribs = [sample(36,t=>stem(t*.92))];
    function pinna(base, side, size, lift) {
        const shape=[];
        for(const sign of [-1,1]){
            const half=sample(26,t=>{
                const e=width*.15*size*Math.sin(Math.PI*t)**.68;
                const jag=toothed ? 1+.20*Math.cos(14*Math.PI*t) : 1;
                return [base[0]+side*size*width*t, base[1]-lift*t-4*size*Math.sin(Math.PI*t)+sign*e*jag];
            });
            shape.push(...(sign===-1?half:half.reverse().slice(1,-1)));
        }
        ribs.push(ring(shape));
    }
    for(let i=0;i<pairs;i++){
        const t=.13+.78*i/(pairs-1), size=Math.sin(Math.PI*(.11+.83*t))**.9*(1-.43*t);
        for(const side of [-1,1]){
            const base=stem(t+(side>0?skew:0));
            pinna(base,side,size,sweep*(.5+size));
        }
    }
    const base=stem(.92);
    ribs.push(...trace('M0 0 Q-4 -5 0 -12 Q4 -6 0 0 Z').map(p=>p.map(([x,y])=>[base[0]+x,base[1]+y])));
    return ribs;
}
const fronds = [
    ['Sword fern frond', [9,9,27,9,0,0]], ['Brake fern', [6,-12,42,13,0,.025]],
    ['Serrate spleenwort', [8,-3,32,7,1,0]], ['Ribbon fern spray', [5,19,36,18,0,.04]],
    ['Narrow marsh fern', [10,2,19,8,1,.025]], ['Broad polypody', [7,-18,35,4,0,.02]],
    ['Leatherleaf frond', [6,0,35,12,1,0]], ['Mountain shield fern', [9,14,31,6,1,.032]],
];

function install(register, add) {
    const slug=name=>name.toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/-$/,'');
    const group=(key,category,items,make)=>{
        register('stock-nature-'+key,()=>{
            for(const [name,...args] of items) {
                const raw=make(...args);
                const paths=raw.map(path=>{
                    const points=path.map(([x,y])=>[Math.round(x*1000)/1000,-Math.round(y*1000)/1000]);
                    return points.filter((p,i)=>i===0||dist(p,points[i-1])>0);
                }).filter(path=>path.length>1);
                add('stock-nature-'+slug(name),name,category,paths);
            }
        });
    };
    const drawn=(strokes,eyes=[])=>[...trace(strokes),...eyes.map(args=>oval(...args))];
    group('ocean','Pelagic wildlife',sea,drawn);
    group('shorebirds','Birds of the shore',shorebirds,drawn);
    group('seedpods','Seed pods & samaras',seeds,drawn);
    group('crystals','Crystal specimens',minerals,crystalCluster);
    group('feathers','Feather studies',feathers,params=>plume(...params));
    group('snow','Frost & ice crystals',frost,snow);
    group('valves','Scallops & bivalves',valves,fanShell);
    group('fossils','Ammonites & chambered shells',coils,ammonite);
    group('seastars','Stars of the reef',stars,seaStar);
    group('sanddollars','Sand dollars & sea biscuits',tests,sandDollar);
    group('agates','Agate cross sections',agates,agate);
    group('ferns','Fern specimens',fronds,params=>frond(...params));
}
if (typeof module !== 'undefined' && module.exports) module.exports=install;
else root.CNCStockNature=install;
})(globalThis);
