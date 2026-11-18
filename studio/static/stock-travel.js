/* Original travel and field illustrations. A contour starts at M; Z closes it.
   Coordinates here follow a sketchbook page, with Y increasing downwards. */
(function (root) {
    'use strict';

    function contours(drawing) {
        const words = drawing.match(/[MLQCZ]|-?(?:\d+(?:\.\d*)?|\.\d+)/g);
        const paths = [];
        let path, i = 0, mode;
        const point = () => [Number(words[i++]), Number(words[i++])];
        while (i < words.length) {
            if (/^[MLQCZ]$/.test(words[i])) mode = words[i++];
            if (mode === 'M') {
                path = [point()];
                paths.push(path);
                mode = 'L';
            } else if (mode === 'Z') {
                path.push(path[0].slice());
                mode = null;
            } else if (mode === 'L') {
                path.push(point());
            } else if (mode === 'C' || mode === 'Q') {
                const a = path[path.length - 1], b = point(), c = point();
                const e = mode === 'C' ? point() : null;
                for (let j = 1; j <= 12; j++) {
                    const t = j / 12, u = 1 - t;
                    path.push(a.map((value, k) => e
                        ? u*u*u*value + 3*u*u*t*b[k] + 3*u*t*t*c[k] + t*t*t*e[k]
                        : u*u*value + 2*u*t*b[k] + t*t*c[k]));
                }
            } else {
                throw Error('Invalid stock travel contour');
            }
        }
        return paths.map(p => p.map(([x, y]) => [x, -y]));
    }

    // Each entry is drawn separately so its outline works on a small badge
    // as well as a larger engraved sign. Open strokes carry the interior detail.
    const collections = {
        'Camp equipment': [
            ['Ridge tent with rolled fly', 'M4 64 L40 16 77 64 Z M40 16 L62 64 M48 64 L43 36 29 64 M5 60 L1 68 M76 61 L83 68 M20 68 L68 68'],
            ['Hurricane lantern', 'M27 29 C14 -2 67 -2 54 29 M21 28 L59 28 62 34 19 34 Z M26 35 C26 45 16 50 27 62 L55 62 C64 50 54 42 55 35 M23 63 L59 63 63 72 19 72 Z M38 58 Q31 52 40 43 Q39 49 44 51 Q48 56 43 58 Z'],
            ['External-frame rucksack', 'M25 20 Q24 11 32 11 L48 11 Q57 11 56 20 M18 27 Q17 18 28 18 L53 18 Q65 18 64 29 L66 61 Q66 70 55 71 L25 71 Q16 70 16 63 Z M18 31 Q40 41 64 31 M32 34 L32 42 36 42 36 35 M46 35 L46 42 50 42 50 34 M26 48 Q40 46 56 48 L55 63 26 63 Z M17 47 L10 48 11 60 16 60 M65 47 L72 49 71 61 66 61'],
            ['Enamel coffee pot', 'M22 30 L52 30 57 66 Q37 72 15 65 L20 45 6 29 12 28 22 37 Z M20 29 Q23 21 35 20 L35 16 40 16 40 20 Q49 21 54 29 M53 34 C77 24 75 60 57 59 M54 40 C66 33 67 51 57 53 M21 61 Q34 65 50 62'],
            ['Rolled sleeping mat', 'M15 33 Q12 20 24 17 L62 9 C79 7 87 34 72 42 L35 59 C20 66 6 45 15 33 Z M16 33 C27 18 44 39 35 53 Q27 63 20 48 Q17 39 24 36 Q32 36 31 46 M40 13 Q58 22 55 49 M45 12 Q63 22 60 47'],
            ['Canvas camp chair', 'M17 17 L54 11 64 43 29 50 Z M30 51 L53 56 76 46 65 43 M34 53 L58 73 M53 57 L30 72 M59 55 L73 69 M72 48 L52 70 M20 21 L28 45 M24 20 L32 44'],
            ['Storm matches in a tin', 'M9 41 L50 23 73 38 31 58 Z M9 41 L9 52 31 69 73 50 73 38 M31 58 L31 69 M20 42 L47 30 M39 56 L65 44 M43 60 L65 50 M13 28 L36 12 Q39 6 34 5 Q29 5 30 10 L10 25 Z M7 17 L20 7 Q21 2 17 2 Q13 2 14 5 L4 14 Z'],
            ['Trail stove and kettle', 'M21 41 Q11 33 23 19 L51 19 Q64 27 55 40 Z M25 18 C18 -2 57 -2 51 18 M24 20 L33 25 47 25 51 20 M58 27 L67 22 69 24 59 36 M21 46 L56 46 52 57 25 57 Z M17 65 Q19 57 27 57 L51 57 Q60 58 62 65 Z M28 49 L28 53 M35 49 L35 53 M42 49 L42 53 M49 49 L49 53']
        ],
        'Coastal navigation': [
            ['Iron screw-pile light', 'M11 43 L69 43 69 48 11 48 Z M22 42 L22 29 40 19 58 29 58 42 M18 30 L40 16 62 30 M34 19 L34 11 46 11 46 19 M31 11 L40 5 49 11 Z M27 48 L24 69 M52 48 L57 69 M28 49 L54 67 M52 50 L26 66 M30 33 L35 33 35 38 30 38 Z M44 43 L44 32 50 32 50 43 M3 70 Q11 66 19 70 M60 70 Q68 66 77 70'],
            ['Striped headland lighthouse', 'M23 63 L31 25 49 25 57 63 Z M28 25 L52 25 52 21 28 21 Z M33 21 L33 13 47 13 47 21 M29 13 L40 5 51 13 Z M29 35 L50 29 M27 44 L52 37 M25 54 L53 46 M35 63 L35 55 Q40 48 45 55 L45 63 M8 68 L20 64 57 64 71 69'],
            ['Bell buoy at anchor', 'M18 48 Q39 41 61 48 L56 56 Q39 60 22 55 Z M26 45 L34 18 47 18 55 45 M28 40 L52 40 M32 28 L49 28 M35 27 L39 21 44 27 M35 15 L46 15 43 10 39 10 Z M39 10 L39 4 M10 61 Q17 56 24 61 M57 62 Q64 56 72 61'],
            ['Harbour signal pennants', 'M17 69 L17 6 20 6 20 69 Z M21 9 Q32 2 46 11 Q59 20 72 11 L62 26 Q51 29 43 22 Q33 14 22 21 Z M23 29 Q32 23 43 30 Q55 38 64 31 L62 45 Q51 50 41 42 Q32 35 23 42 Z M9 70 L28 70'],
            ['Box compass rose', 'M40 3 L46 31 76 38 47 45 40 75 33 46 5 38 33 31 Z M40 3 L40 38 46 31 M76 38 L40 38 47 45 M40 75 L40 38 33 46 M5 38 L40 38 33 31 M22 27 L16 14 29 24 M52 24 L65 14 59 28 M58 50 L65 62 52 56 M29 55 L16 63 22 50'],
            ['Admiralty anchor emblem', 'M37 20 C23 17 28 2 40 3 C53 3 57 17 43 20 L43 29 57 29 57 34 43 34 43 59 Q56 58 63 46 L58 46 74 34 72 53 68 49 Q60 68 40 74 Q20 67 12 49 L8 53 6 34 22 46 17 46 Q24 58 37 59 L37 34 23 34 23 29 37 29 Z M40 9 C34 9 34 16 40 16 C46 16 46 9 40 9 Z'],
            ['Folding marine telescope', 'M7 46 L28 34 37 50 16 62 Z M30 35 L49 24 57 38 38 49 M50 24 L65 15 73 29 57 38 M64 15 L69 12 78 28 73 31 Z M7 45 L3 48 13 65 17 63 Z M25 40 L31 50'],
            ['Tide gauge staff', 'M32 6 L46 6 46 62 32 62 Z M33 14 L42 14 M33 20 L38 20 M33 26 L42 26 M33 32 L38 32 M33 38 L42 38 M33 44 L38 44 M33 50 L42 50 M4 56 Q12 50 20 56 Q26 60 31 56 M47 56 Q55 51 61 56 Q68 62 77 55 M7 68 Q17 61 27 67 Q36 74 47 67 Q55 62 64 67']
        ],
        'Aerostat studies': [
            ['Montgolfière scalloped envelope', 'M38 58 C13 44 6 24 17 13 C27 2 53 2 63 14 C74 28 62 46 44 58 Z M38 57 Q16 30 28 10 M42 56 Q60 27 49 9 M15 27 Q20 36 26 28 Q32 38 40 29 Q48 38 55 28 Q62 35 66 27 M36 60 L38 68 44 68 46 60 M36 69 L46 69 45 74 37 74 Z'],
            ['Gas balloon in a rope net', 'M37 57 C7 44 8 11 32 7 C66 -1 79 36 46 57 Z M27 11 Q15 32 37 53 M40 8 L41 54 M51 10 Q66 31 46 53 M16 24 L23 30 31 24 40 31 51 24 57 30 64 23 M19 38 L27 43 33 38 41 43 50 38 54 42 60 37 M35 58 L37 66 45 66 48 58 M35 67 L47 67 46 74 36 74 Z'],
            ['Observation airship', 'M8 32 C18 13 54 10 70 27 L80 20 77 33 81 44 69 39 C50 53 19 49 8 32 Z M11 32 Q39 18 66 29 M13 34 Q40 43 68 35 M31 47 L35 53 48 53 53 46 M35 54 L49 54 46 60 36 60 Z M61 34 L71 29 71 36 Z'],
            ['Striped windsock', 'M11 72 L11 16 14 16 14 72 Z M16 19 L26 14 M16 21 L26 32 M28 14 C24 16 24 29 29 33 Q44 29 54 36 Q62 42 76 38 L76 32 Q66 33 59 27 Q48 17 28 14 Z M36 16 Q32 22 35 31 M45 19 Q41 25 43 31 M55 25 Q51 29 53 34 M64 30 L61 38'],
            ['Delta kite and bow tail', 'M5 47 L41 6 77 47 45 37 40 57 33 37 Z M41 8 L40 54 M9 45 L40 30 73 45 M41 57 Q43 61 51 62 Q68 64 62 76 M49 61 L45 57 44 63 Z M52 62 L56 59 57 65 Z M62 70 L57 66 57 72 Z M63 70 L68 68 68 73 Z'],
            ['Diamond survey kite', 'M40 4 L67 26 40 60 13 26 Z M40 6 L40 58 M15 26 Q40 20 65 26 M40 60 Q31 67 40 70 Q48 72 44 78 M34 66 L29 63 29 69 Z M36 67 L41 64 42 69 Z']
        ],
        'Travellers’ keepsakes': [
            ['Railway ticket punch', 'M8 23 L73 23 73 33 Q63 37 73 42 L73 56 8 56 8 44 Q17 39 8 34 Z M54 27 L54 32 M54 36 L54 42 M54 46 L54 52 M18 32 L43 32 M18 39 L37 39 M18 47 L45 47 M62 31 L66 31 M62 48 L66 48'],
            ['Leather valise', 'M6 24 Q6 20 11 20 L68 20 Q74 20 74 26 L74 59 Q74 64 69 64 L11 64 Q6 64 6 59 Z M30 19 L30 13 Q30 9 35 9 L46 9 Q51 9 51 14 L51 19 M34 19 L34 14 47 14 47 19 M19 21 L19 63 M24 21 L24 63 M56 21 L56 63 M61 21 L61 63 M8 31 Q16 29 16 21 M64 21 Q65 29 73 31 M7 53 Q16 55 16 63 M64 63 Q65 55 73 53'],
            ['Hotel desk bell', 'M12 53 Q14 30 37 27 L37 22 34 20 Q32 15 40 15 Q48 15 46 20 L43 22 43 27 Q65 29 68 53 Z M9 56 L71 56 74 62 6 62 Z M20 46 Q24 34 35 32'],
            ['Folding road atlas', 'M5 21 L26 13 51 20 75 12 75 61 51 69 27 61 5 69 Z M26 14 L27 60 M51 21 L51 68 M8 55 Q19 49 18 38 Q18 31 24 29 M29 28 Q39 26 42 36 Q44 42 49 42 M54 41 Q66 37 71 20 M9 29 L14 27 M32 52 L40 47 M58 55 L68 52'],
            ['Round luggage label', 'M31 19 L38 7 50 6 56 17 C81 30 73 62 52 69 C27 77 6 51 14 32 Q19 22 31 19 Z M41 12 Q47 8 48 13 Q48 18 43 17 Q39 17 41 12 Z M22 47 L33 33 41 43 49 30 65 49 M20 54 Q40 59 63 54'],
            ['Instant travel photograph', 'M10 9 L71 14 66 72 5 67 Z M15 15 L65 19 62 53 12 49 Z M14 43 L27 29 38 41 47 33 61 48 M20 57 L44 59 M51 24 Q55 21 58 25 Q60 30 55 31 Q50 31 51 24 Z'],
            ['Postmark and envelope', 'M4 18 L74 18 74 62 4 62 Z M5 19 L38 43 57 31 M5 61 L28 40 M73 61 L52 40 M60 21 L70 21 70 31 60 31 Z M43 23 Q49 20 54 23 M43 26 Q49 23 54 26'],
            ['Wax-sealed letter', 'M6 17 L74 17 74 63 6 63 Z M7 18 L32 38 M73 18 L48 38 M7 62 L31 42 M73 62 L49 42 M30 39 L32 34 37 34 41 31 45 34 49 35 48 40 50 44 45 46 41 49 37 46 32 46 Z M36 40 Q39 35 44 38 Q47 42 41 44 Q38 45 36 40 Z']
        ],
        'Astronomy equipment': [
            ['Equatorial telescope', 'M12 35 L52 11 61 25 21 49 Z M50 11 L55 8 65 24 60 27 Z M14 38 L8 42 12 49 18 46 M32 45 L38 42 44 50 37 55 Z M39 55 L25 73 M40 55 L40 73 M42 55 L58 72 M24 33 L29 41'],
            ['Radio dish array', 'M10 19 Q18 58 54 51 Z M14 22 L47 15 52 46 M46 15 L49 11 53 14 50 18 Z M32 47 L27 62 41 62 44 52 M21 67 L48 67 52 72 16 72 Z M59 41 Q62 55 76 56 Z M69 55 L67 64 71 64 M61 66 L76 66'],
            ['Slit-dome observatory', 'M8 46 Q9 15 40 10 Q69 12 72 46 Z M34 42 Q34 23 44 14 M42 43 Q41 26 51 17 M9 49 L71 49 71 66 9 66 Z M16 54 L26 54 26 60 16 60 Z M31 54 L41 54 41 60 31 60 Z M49 66 L49 54 62 54 62 66 M4 69 L76 69'],
            ['Pocket star wheel', 'M12 14 L65 14 73 48 64 66 16 66 7 47 Z M19 34 C25 15 54 17 62 34 Q44 48 19 34 Z M19 48 Q39 64 62 48 M23 50 L21 53 M32 53 L31 57 M41 54 L41 58 M50 53 L52 57 M58 50 L61 53 M27 31 L32 28 39 32 46 25 52 28 M18 60 L25 60'],
            ['Orbital station module', 'M29 29 L48 29 52 35 52 46 47 51 30 51 25 45 25 35 Z M18 36 L24 36 M18 44 L24 44 M53 36 L59 36 M53 44 L59 44 M2 19 L18 19 18 59 2 59 Z M59 19 L76 19 76 59 59 59 Z M10 20 L10 58 M3 32 L17 32 M3 45 L17 45 M67 20 L67 58 M60 32 L75 32 M60 45 L75 45 M30 34 L46 34 46 45 30 45 Z M36 29 L36 20 42 20 42 29'],
            ['Lunar lander study', 'M27 24 L33 15 46 15 54 24 51 39 29 39 Z M31 25 L36 20 40 26 Z M44 20 L50 25 44 27 Z M23 41 L57 41 59 52 21 52 Z M25 53 L15 66 8 66 M29 54 L18 66 M53 54 L64 66 72 66 M50 54 L61 66 M36 53 L33 59 46 59 43 53 M39 14 L39 9 47 5 M19 68 L8 68 M60 68 L72 68'],
            ['Meteor camera', 'M17 46 L63 46 66 62 14 62 Z M23 44 Q23 19 40 18 Q57 19 57 44 Z M30 43 Q29 28 38 23 M21 52 L35 52 35 57 21 57 Z M49 55 L58 55 M24 63 L21 69 M56 63 L60 69 M57 14 L68 5 M61 17 L72 8'],
            ['Armillary sundial', 'M39 7 C77 7 77 57 40 58 C5 59 6 8 39 7 Z M19 46 C17 30 43 12 61 17 C58 31 35 50 19 46 Z M28 13 Q20 36 51 53 M24 51 L56 15 M36 59 L34 64 46 64 44 59 M27 66 L53 66 60 71 20 71 Z']
        ]
    };

    function install(register, add) {
        for (const [category, designs] of Object.entries(collections)) {
            const key = category.toLowerCase().replace(/[^a-z]+/g, '-');
            register('stock-travel-' + key, () => {
                for (const [name, drawing] of designs) {
                    const id = name.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[^a-z0-9]+/g, '-');
                    add('stock-travel-' + id, name, category, contours(drawing));
                }
            });
        }
    }
    if (typeof module !== 'undefined') module.exports = install;
    else root.CNCStockTravel = install;
})(globalThis);
