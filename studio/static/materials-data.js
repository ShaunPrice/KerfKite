// Material selection for the Studio heads. Source links describe a process or a
// manufacturer's own equipment. A candidate still needs a head-specific test.
(() => {
  'use strict';

  const sources = {
    compression: {
      title: 'LMT Onsrud · opposing flutes on veneered sheet',
      url: 'https://onsrud.com/Products/68101L.asp'
    },
    chipDirection: {
      title: 'LMT Onsrud · flute geometry and chip removal in plastics',
      url: 'https://onsrud.com/articles/The-Router-Way.asp'
    },
    bladeOffset: {
      title: 'Roland DG · blade angle and tip offset',
      url: 'https://www.rolanddga.com/en-la/blog/what-should-my-cutter-settings-be'
    },
    acetal: {
      title: 'Delrin · acetal thermal decomposition, page 6',
      url: 'https://www.delrin.com/wp-content/uploads/2024/11/Delrin-Technical-Molding-Guide-NA-FNL.pdf#page=6'
    },
    cutterShapes: {
      title: 'Vectric · tool database and cutter geometry',
      url: 'https://docs.vectric.com/docs/V12.0/VCarvePro/ENU/Help/form/Tool%20Database/index.html'
    },
    router: {
      title: 'Carbide 3D · Shapeoko router material capabilities',
      url: 'https://shop.carbide3d.com/products/shapeoko-pro-cnc-router'
    },
    milling: {
      title: 'Carbide 3D · Nomad material guide, page 21',
      url: 'https://my.carbide3d.com/pdf/Nomad3_Getting_Started_Guide_12_22_2020_v1_smaller.pdf#page=21'
    },
    dust: {
      title: 'UK Health and Safety Executive · wood dust control',
      url: 'https://www.hse.gov.uk/PUBNS/wis23.htm'
    },
    diode: {
      title: 'xTool · material response with a blue diode on the M2',
      url: 'https://support.xtool.com/article/3442'
    },
    reflection: {
      title: 'xTool · blue diode materials and reflective surfaces',
      url: 'https://support.xtool.com/article/3324'
    },
    acrylic: {
      title: 'xTool · wavelength and acrylic colour',
      url: 'https://support.xtool.com/article/555'
    },
    excluded: {
      title: 'Trotec · materials to exclude from laser processing',
      url: 'https://www.troteclaser.com/en-us/resources/faqs/unsuitable-materials-laser-processing'
    },
    laserSafety: {
      title: 'MIT Environment, Health & Safety · laser cutter guidance',
      url: 'https://ehs.mit.edu/wp-content/uploads/MITEHS_Laser_Cutter_Guidance.pdf'
    },
    blades: {
      title: 'Cricut · blade geometry and compatible stock',
      url: 'https://help.cricut.com/hc/en-us/articles/360009432294-All-About-Cricut-Blades-and-Machine-Tools'
    },
    ironOn: {
      title: 'Cricut · carrier orientation and mirroring iron-on designs',
      url: 'https://help.cricut.com/hc/en-us/articles/360009432354-How-to-mirror-your-design-in-Design-Space'
    }
  };

  const tools = [
    { id: 'flat', family: 'router', label: 'Flat end mill', operations: [
      { id: 'profile', label: 'Profile cut' }, { id: 'pocket', label: 'Flat-bottom pocket' },
      { id: 'engrave', label: 'Centreline engraving' }, { id: 'relief', label: '3D relief roughing' }
    ] },
    { id: 'ball', family: 'router', label: 'Ball-nose end mill', operations: [
      { id: 'relief', label: '3D relief finishing' }
    ] },
    { id: 'v', family: 'router', label: 'V-bit', operations: [
      { id: 'engrave', label: 'V-carve or engrave' }
    ] },
    { id: 'diode', family: 'laser', label: 'Blue diode laser', operations: [
      { id: 'engrave', label: 'Surface engrave' }, { id: 'cut', label: 'Through cut' }
    ] },
    { id: 'knife', family: 'knife', label: 'Vinyl drag knife', operations: [
      { id: 'kiss', label: 'Kiss cut to liner' }, { id: 'through', label: 'Through cut' }
    ] }
  ];

  const toolById = Object.fromEntries(tools.map(tool => [tool.id, tool]));
  const candidate = 'candidate', conditional = 'conditional', avoid = 'avoid';
  const op = (status, advice) => ({ status, advice });

  function entry(summary, operations, setup, inspect, sources) {
    // The overview describes the most promising operation. Each operation has
    // its own verdict, so a surface-marking option cannot authorise a through cut.
    const states = Object.values(operations).map(operation => operation.status);
    const status = [candidate, conditional, avoid].find(value => states.includes(value));
    return { status, summary, operations, setup, inspect, sources };
  }

  function excludedTool(toolId, reason, refs) {
    const operations = Object.fromEntries(toolById[toolId].operations.map(({ id }) => [id, op(avoid, reason)]));
    return entry(reason, operations, ['Set this stock aside for this tool.'], ['Check the stock label before choosing another process.'], refs);
  }

  function excludeRouting(reason, refs = ['cutterShapes']) {
    return Object.fromEntries(['flat', 'ball', 'v'].map(id => [id, excludedTool(id, reason, refs)]));
  }

  function routerOptions(status, stockNote, sampleCheck, refs) {
    return {
      flat: entry('The square end makes vertical walls and a level floor.', {
        profile: op(status, 'Allow for the cutter radius at inside corners. Plan how the part will stay held as the outside edge opens.'),
        pocket: op(status, 'Measure the smallest recess against the cutter diameter and check the floor depth on scrap.'),
        engrave: op(status, 'The bit cuts a line at least as wide as its diameter. Check the centres of neighbouring strokes for overlap.'),
        relief: op(status, 'Select flat cutter contact in the relief preview. Look for stepped slopes and narrow valleys that will need a smaller finishing tool.')
      }, [stockNote, 'Check cutting-edge length against the deepest cut and the collet against the clamps.'],
      [sampleCheck, 'Measure a slot or pocket after removing the chips.'], refs),
      ball: entry('A rounded tip follows sloped surfaces and leaves a radius in valleys.', {
        relief: op(status, 'Use the relief preview to check that the tool radius can reach the narrowest valley. Test the surface finish on a representative slope.')
      }, [stockNote, 'Choose a ball radius that fits the concave details and leave room for the holder.'],
      [sampleCheck, 'Look across the contour in raking light for ridges from the space between passes.'], refs),
      v: entry('A V-bit widens the line as its tip goes deeper.', {
        engrave: op(status, 'Check the included angle marked on the bit and preview the deepest point of wide lettering.')
      }, [stockNote, 'Reference Z to the actual top face. Bow or local thickness changes alter the stroke width.'],
      [sampleCheck, 'Compare a narrow stroke and a wide stroke with the drawing.'], refs)
    };
  }

  const wood = (note, check, status = candidate) => routerOptions(status, note, check, ['cutterShapes', 'dust', 'router']);
  function plastic(note, check, status = conditional) {
    const routes = routerOptions(status, note, check, ['milling', 'cutterShapes']);
    routes.flat.setup.push('A plastics-rated upcut carries chips out of a recess. A downcut presses stock towards the bed but can trap chips in pockets.');
    routes.flat.sources = [...routes.flat.sources, 'chipDirection'];
    return routes;
  }
  const metal = (note, check) => routerOptions(conditional, note, check, ['router', 'cutterShapes']);
  const sheerStock = excludeRouting('Flexible stock can lift, tear or wrap around a rotating cutter. Use a blade suited to the sheet.');
  const hardKnife = excludedTool('knife', 'A swivelling vinyl blade cannot follow a through cut or a liner in this rigid stock.', ['blades']);
  const noDiode = (reason, refs = ['laserSafety']) => excludedTool('diode', reason, refs);
  const readStock = 'Identify every layer, including ink, adhesive, masking and finish. Check approval for the exact sheet and head.';
  const fireSetup = 'Use the laser enclosure and extraction specified for the head. Attend the entire job, including the test piece.';

  function laser(engrave, cut, summary, note, check, refs = ['diode', 'laserSafety']) {
    return entry(summary, { engrave, cut }, [note, fireSetup], [check], refs);
  }

  function woodLaser(note, mark, cut, status = candidate) {
    return laser(op(status, mark), op(conditional, cut),
      'Surface contrast tracks the grain. Through cutting depends on stock thickness and the specific head.',
      note, 'Look at both faces once the sample is cool. A dark top line alone does not show that the cut separated.', ['reflection', 'diode', 'laserSafety']);
  }

  function paperLaser(note) {
    return laser(op(conditional, 'A wide fill can burn a hole before it leaves an even mark. A knife is an alternative for outlines.'),
      op(conditional, 'Test a simple contour on the same paper. Watch small islands, loose scraps and the underside.'),
      'Paper responds quickly and small offcuts can ignite.', note, 'Check for browned edges, loose glowing fibres and incomplete corners.');
  }

  const knifeGeometry = 'Use the blade maker’s angle for the stock and enter the tip offset for that exact blade. Equal blade angles can have different offsets.';
  function knifeSheet(note, check, status = candidate) {
    return entry('An unlined sheet needs support beneath a through cut.', {
      kiss: op(avoid, 'There is no release liner to stop at. Use a through cut on a blade-rated mat.'),
      through: op(status, 'Test a closed shape with an inside corner. Leave room outside the design for the knife alignment lead.')
    }, [note, knifeGeometry, 'Check the holder’s blade reach and use a suitable sacrificial cutting mat.'],
    [check, 'Lift the sample and inspect the mat for deep scoring.'], ['blades', 'bladeOffset']);
  }

  function knifeLiner(note, check, refs = ['blades']) {
    return entry('Use a kiss cut to weed a design on its liner or a through cut to release a backed shape.', {
      kiss: op(candidate, 'Place a small corner and an enclosed counter in a scrap area. Weed both while the design stays on its liner.'),
      through: op(conditional, 'For a separate backed shape, support the entire liner on a cutting mat. Check the total stack against blade reach.')
    }, [note, knifeGeometry, 'Check the lead and swivel path in the preview. They can mark the waste beside the part.'],
    [check, 'For a kiss cut, check that the face peels from an intact liner. For a through cut, check that the backed piece separates and the mat is free of deep scoring.'], [...refs, 'bladeOffset']);
  }

  const groups = {
    wood: 'Solid wood', boards: 'Engineered wood', plastics: 'Rigid plastics',
    paper: 'Paper and board', films: 'Films and laminates', soft: 'Leather and textiles',
    metals: 'Metals', other: 'Specialty stock'
  };
  function material(id, name, group, aliases, description, router, diode, knife = hardKnife) {
    return { id, name, group: groups[group], aliases, description, tools: { ...router, diode, knife } };
  }

  const noFilmLaser = noDiode('The adhesive, liner and face all enter the beam path. Use a knife unless the complete product has a documented laser process.', ['excluded', 'laserSafety']);
  const pvcLaser = noDiode('Keep PVC away from the laser. Heating it releases corrosive hydrogen chloride.', ['laserSafety', 'excluded']);
  const noReflectiveLaser = noDiode('The exposed metal can reflect the blue beam. Use a documented mechanical process for this surface.', ['reflection']);
  const noSoftKnife = excludedTool('knife', 'This stock exceeds the intended use of a vinyl blade. Choose a rigid-stock cutting process.', ['blades']);
  const acrylicRouter = plastic('Support the whole sheet and use a sharp cutter intended for acrylic with space for chips to leave.',
    'Look for small fractures at corners and chips welded back onto the edge.', candidate);
  const plywoodTools = wood('Support the exit face and capture wood dust. Check a cut edge for inner voids.',
    'Check both faces for splintering and the pocket for lifted veneer.');
  plywoodTools.flat.setup.push('A downcut helps keep the top veneer intact. A compression cutter protects both faces when its upcut and downcut sections actually engage the respective plies.');
  plywoodTools.flat.sources = [...plywoodTools.flat.sources, 'compression'];
  const woodExtract = 'Capture wood dust at the cutter and support the grain across the toolpath.';

  const materials = [
    material('basswood', 'Basswood / lime', 'wood', ['linden', 'limewood', 'tilia'],
      'A pale, fine-grained blank for lettering and relief carving. This entry covers solid untreated timber.',
      wood(woodExtract, 'Check fine ridges for crushed fibres and fuzzy corners.'),
      woodLaser('Confirm the blank is untreated solid wood and check it for warp.', 'Compare fine lines and a small filled area on the same face.',
        'Use only stock within the head maker’s published cutting capability. Confirm separation on a waste contour.')),
    material('pine', 'Pine', 'wood', ['radiata', 'softwood', 'pinus'],
      'Soft earlywood and harder growth rings can produce alternating light and dark bands.',
      wood('Choose a flat, knot-free test area and capture the dust at source.', 'Cross a growth ring on the sample to reveal tear-out or resin smearing.'),
      woodLaser('Choose an untreated face free of resin pockets, knots and finish.', 'Run the engraving across several growth rings to assess contrast.',
        'Pitch pockets and knots change the cut. Avoid them in the part and its test piece.')),
    material('maple', 'Maple', 'wood', ['hard maple', 'acer'],
      'A close-grained hardwood for small type, mouldings and shallow reliefs.',
      wood(woodExtract, 'Inspect the end-grain edge for burnishing or burn marks.'),
      woodLaser('Use a bare, unfinished maple blank.', 'Compare the palest area with the darkest portion of the sample.',
        'Check this hardwood species and thickness against the head documentation.')),
    material('oak', 'Oak', 'wood', ['white oak', 'red oak', 'quercus'],
      'Open pores and prominent grain can interrupt a very fine engraved line.',
      wood(woodExtract, 'Examine small islands of end grain for chips or missing corners.'),
      woodLaser('Test both along and across the pores on untreated stock.', 'Inspect fine text across the growth rings before committing the layout.',
        'Thick hardwood is a poor choice for a diode through cut. Check the exact head and sheet thickness first.')),
    material('walnut', 'Walnut', 'wood', ['black walnut', 'juglans'],
      'A dark hardwood. Colour changes between heartwood and sapwood affect the legibility of a mark.',
      wood(woodExtract, 'Check the smallest raised letter for crumbling and compare both grain directions.'),
      woodLaser('Use an offcut from the same part of the board.', 'Judge contrast on the darkest section of the intended face.',
        'Confirm the head maker supports through cutting this thickness and species.')),
    material('plywood', 'Birch plywood', 'boards', ['birch ply', 'laser plywood', 'laminated ply'],
      'Thin wood veneers laminated with adhesive. The glue line and inner plies can differ between visually identical sheets.',
      plywoodTools,
      laser(op(conditional, 'Engrave only a sheet with a documented laser-compatible adhesive and bare face.'),
        op(conditional, 'Use a laser-compatible grade. An interior void or glue pocket can leave an uncut bridge.'),
        'The adhesive specification matters even for a surface engraving.', readStock, 'Look along the entire edge and inspect the underside for dark glue pockets.')),
    material('mdf', 'Medium-density fibreboard', 'boards', ['mdf', 'fiberboard', 'fibreboard'],
      'Compressed wood fibres and resin. Laser-rated board and building-grade board can have different binders.',
      wood('Use effective fine-dust extraction at the cutter and check the board for surface coatings.',
        'Look for fuzzy edges at the lip and crumbly detail in the exposed core.'),
      laser(op(conditional, 'Select an identified laser-rated grade before testing surface text.'),
        op(conditional, 'Confirm the specific resin and grade are approved for cutting. Check the darkened edge for a continuous through cut.'),
        'A generic MDF label does not identify the binder.', readStock, 'Inspect the full edge and both board faces for charring and incomplete separation.',
        ['excluded', 'laserSafety'])),
    material('bamboo', 'Laminated bamboo', 'boards', ['bamboo board', 'bamboo ply'],
      'Joined strips of dense bamboo. A glue seam can cross a small letter or interrupt a shallow relief.',
      wood('Identify the binder and surface finish. Support the strips across their long grain and collect dust.',
        'Inspect the transitions between strips for splintering or separated glue lines.', conditional),
      laser(op(conditional, 'Only engrave material whose strips, adhesive and finish have a documented laser process.'),
        op(conditional, 'Check the manufacturer’s limits for the entire laminate and measure separation at a glue line.'),
        'Face colour varies between strips and glue lines change the response.', readStock,
        'Compare a mark on a strip with one crossing a joint.', ['reflection', 'excluded', 'laserSafety'])),
    material('veneer', 'Unbacked wood veneer', 'boards', ['veneer sheet', 'marquetry'],
      'A single thin slice of timber. Paper, adhesive or fleece backing changes all three processes.',
      wood('Support the entire veneer on a carrier suitable for routing. Keep the cutter clear of fasteners.',
        'Look for lifting between the carrier and the sheet.', conditional),
      woodLaser('Use bare, unbacked veneer and check that the whole sheet lies flat.',
        'Use a sample with the same grain direction as the work.', 'Confirm focus and check for heat marks on the underside.', conditional),
      knifeSheet('Check that the holder maker approves this veneer thickness. Keep a short-grain feature on the test piece.',
        'Watch for splitting along the grain at tight corners.', conditional)),
    material('dark-acrylic', 'Dark opaque cast acrylic', 'plastics', ['black acrylic', 'opaque PMMA', 'cast perspex'],
      'Cast PMMA coloured throughout. Dark appearance alone does not establish blue-light absorption.',
      acrylicRouter,
      laser(op(conditional, 'Verify this exact colour and casting grade is sold for blue-diode engraving. Compare the mark with the unmarked face.'),
        op(conditional, 'Confirm the grade and thickness against the head maker’s data. A coloured masking film cannot establish through-cut suitability.'),
        'Some dark cast PMMA absorbs blue light. Pigment, head and thickness determine the result.',
        'Identify PMMA and account for any paper or plastic masking before testing.', 'Check for rejoined edges, bubbles and uneven contrast.',
        ['acrylic', 'laserSafety'])),
    material('clear-acrylic', 'Clear cast acrylic', 'plastics', ['clear PMMA', 'perspex', 'plexiglass', 'transparent acrylic'],
      'Transparent cast PMMA is a practical mechanical-routing stock. It transmits much of a blue diode beam.', acrylicRouter,
      noDiode('A blue diode passes through clear PMMA and cannot directly make a dependable cut or surface mark. Route the sheet or use a process qualified for clear acrylic.', ['acrylic'])),
    material('polycarbonate', 'Polycarbonate', 'plastics', ['pc', 'lexan', 'makrolon'],
      'A tough transparent engineering plastic. Identify the sheet by its product marking or invoice.',
      plastic('Use a cutter rated for polycarbonate. Support the sheet without bending it.', 'Inspect for burrs, smeared chips and whitening at clamped edges.'),
      noDiode('Exclude polycarbonate from this blue-diode guide. Poor cutting and heavy soot make this an unsuitable workshop process.', ['laserSafety'])),
    material('hdpe', 'HDPE sheet', 'plastics', ['high density polyethylene', 'pehd', 'HDPE cutting board'],
      'A tough, flexible polyethylene sheet. It can bow under clamp pressure and leave long burrs.',
      plastic('Use a cutter intended for soft plastic and a hold-down that keeps the sheet level.', 'Check dimensions after unclamping and look for strings of plastic in the cut.'),
      noDiode('There is no qualified blue-diode process here for this grade of polyethylene. A mechanical cut avoids melting the contour.')),
    material('acetal', 'Acetal / POM', 'plastics', ['delrin', 'polyoxymethylene', 'acetal resin'],
      'An engineering plastic used for spacers and gears. The grade and any fillers should be identified before machining.',
      plastic('Check the supplier’s machining guidance and use a cutter suited to acetal.', 'Measure critical fits after the part cools and look for raised burrs.'),
      noDiode('This guide covers mechanical cutting of acetal. Keep it out of the blue diode because thermal decomposition of acetal can release formaldehyde.', ['acetal', 'laserSafety'])),
    material('rigid-pvc', 'Rigid PVC sheet', 'plastics', ['polyvinyl chloride', 'uPVC', 'PVC board'],
      'Rigid polyvinyl chloride is distinct from the flexible sign film sold on a liner.',
      plastic('Verify the exact PVC grade and mechanical machining guidance. Use chip removal that prevents heat build-up.',
        'Inspect for smearing or local softening and stop if the cut overheats.'), pvcLaser),
    material('cardstock', 'Plain cardstock', 'paper', ['card stock', 'kraft card', 'bristol board', 'paperboard'],
      'Uncoated, unlined card for packaging mock-ups, stencils and folded paper designs.', sheerStock,
      paperLaser('Use plain, identified card. Foil, glitter, coatings and laminates require a separate assessment.'),
      knifeSheet('Support the entire card on a mat that releases it without tearing the face.', 'Check narrow bridges and lift the card without curling it.')),
    material('paper', 'Uncoated paper', 'paper', ['kraft paper', 'drawing paper', 'copier paper'],
      'A thin, unlined sheet suited to silhouettes and stencils. Fine points tear easily as the sheet lifts.', sheerStock,
      paperLaser('Check that the sheet is uncoated and stays flat in the specified airflow.'),
      knifeSheet('Choose a low-tack support suited to the paper and flatten curls before placing the sheet.',
        'Check that the cut releases cleanly without fibres pulling from the top face.')),
    material('corrugated-card', 'Corrugated cardboard', 'paper', ['corrugated board', 'fluted cardboard', 'packing box'],
      'Paper faces separated by flutes. The voids can conceal smouldering fibres.', sheerStock,
      paperLaser('Use identified plain board without tape, labels or staples. Check the flutes for debris and attend the full cut.'),
      knifeSheet('Measure the full sandwich. Use only a blade and holder rated for that thickness and avoid crushing the flutes.',
        'Check the underside for an uncut face and examine corners for crushing.', conditional)),
    material('pvc-vinyl', 'PVC adhesive sign vinyl', 'films', ['adhesive vinyl', 'signwriting vinyl', 'decal vinyl', 'PVC film'],
      'A thin PVC face on a peel-away liner for lettering, labels and decals.', sheerStock, pvcLaser,
      knifeLiner('Lay the liner flat beneath the face film. Keep the corners of the sheet out of the blade path.', 'Weed a small letter and inspect its inside corner.')),
    material('heat-transfer', 'Heat-transfer film', 'films', ['HTV', 'iron-on vinyl', 'shirt transfer'],
      'A garment-transfer film on a carrier. Metallic, printable and patterned products can use different layer orders.', sheerStock, noFilmLaser,
      knifeLiner('Follow the manufacturer’s carrier orientation. For reverse-cut products, mirror lettering and place the carrier underneath.',
        'Read a test word in its final transfer orientation before cutting the full design.', ['blades', 'ironOn'])),
    material('label-paper', 'Adhesive label paper', 'films', ['sticker paper', 'label sheet', 'paper stickers'],
      'A printable paper face, adhesive and release liner. The liner should stay intact when producing a sheet of labels.', sheerStock, noFilmLaser,
      knifeLiner('Identify the printable face and carrier. Allow printed artwork to dry before placing it on the mat.',
        'Peel a single label and check that neighbouring stickers stay on the liner.')),
    material('stencil-film', 'Unlined polyester stencil film', 'films', ['PET stencil', 'mylar stencil', 'polyester film'],
      'A reusable, unlined stencil sheet. This entry covers bare film without adhesive or metallisation.', sheerStock,
      noDiode('Clear stencil film has no qualified direct blue-diode process in this guide. Use the mechanical blade route.'),
      knifeSheet('Confirm the film thickness fits the holder. Support bridges and leave room for the alignment lead.',
        'Lift the stencil and check both sides of each narrow bridge.', conditional)),
    material('veg-leather', 'Natural vegetable-tanned leather', 'soft', ['veg tan', 'vegetable tanned hide', 'tooling leather'],
      'Bare leather with a documented vegetable tanning process. Dyed, coated and bonded products have different chemistry.', sheerStock,
      laser(op(conditional, 'Engrave only a traceable, unfinished hide approved for laser use.'),
        op(conditional, 'Check tannage, surface treatment and thickness against the material and head suppliers.'),
        'Tanning agents and finishing chemicals determine laser eligibility.', readStock,
        'Check the edge and the flexibility of a marked section after it cools.', ['excluded', 'diode', 'laserSafety']),
      knifeSheet('Check the complete hide thickness against the holder rating and restrain stretchy areas on a compatible mat.',
        'Look for stretching at inside corners and fibres still joined on the flesh side.', conditional)),
    material('faux-leather', 'Synthetic or coated leather', 'soft', ['pleather', 'leatherette', 'faux leather', 'bonded leather'],
      'An imitation hide or coated sheet that may contain PVC, polyurethane, binders or a textile base.', sheerStock,
      noDiode('Composition, tanning and coatings can be uncertain. Keep this material out of the laser, including any product containing PVC.', ['excluded', 'laserSafety']),
      knifeSheet('Verify the backing and total thickness. Use a holder approved for the sheet and check the soft face will release from the mat.',
        'Look for a clean cut through the backing and cracking in the finish.', conditional)),
    material('cotton', 'Plain woven cotton', 'soft', ['calico', 'canvas', 'cotton cloth'],
      'An untreated woven fabric. A freely swivelling blade can push threads ahead of it.', sheerStock,
      laser(op(conditional, 'Use only a fully identified, untreated fabric approved for the exact laser. Engraved threads can lose strength.'),
        op(conditional, 'Confirm the cloth can be held flat and will stay clear of the head in the extraction airflow.'),
        'Dyes, waterproofing and fire treatments change the laser assessment.', readStock,
        'Check the perimeter for loose or smouldering fibres and inspect pulled threads.', ['diode', 'excluded', 'laserSafety']),
      knifeSheet('Use only if the holder maker supports the weave and a compatible stabilising carrier. A loose weave shifts under a drag blade.',
        'Inspect curves across and along the weave for dragged threads.', conditional)),
    material('bare-aluminium', 'Bare aluminium alloy', 'metals', ['aluminum', '6061', 'aluminium sheet'],
      'Identified non-ferrous alloy without a surface coating. Reflective faces require particular care when selecting a process.',
      metal('Confirm your router’s metal capability, an alloy-rated cutter, rigid workholding and chip management.',
        'Look for burrs and material welded to the edge of the cutter.'), noReflectiveLaser),
    material('anodised-aluminium', 'Dark anodised aluminium', 'metals', ['anodized aluminum', 'anodised faceplate', 'black anodized tag'],
      'An aluminium substrate with an anodised surface. The coloured layer and the underlying metal behave differently.',
      metal('Check the substrate alloy and protect the finished face while clamping.', 'Inspect the transition from the anodised coating to bare metal for burrs.'),
      laser(op(conditional, 'Only use a specifically approved, non-mirror, dark anodised product on a head documented for surface marking.'),
        op(avoid, 'A visible mark in the anodised surface does not establish penetration through the metal. Select a mechanical profile process.'),
        'A qualified diode process can change the dark surface while leaving the metal intact.',
        'Verify the surface is anodised and confirm the head maker’s instructions for managing exposed reflective metal.',
        'Check contrast and whether the finished mark rubs off after cooling.', ['reflection', 'laserSafety'])),
    material('brass', 'Lead-free machinable brass', 'metals', ['lead free brass', 'brass plate', 'brass nameplate'],
      'Known lead-free alloy for small plates and decorative work. The alloy and machine rigidity matter.',
      metal('Confirm the alloy is lead-free and your router and cutter maker support it. Use secure workholding suited to metal.',
        'Inspect thin areas for movement and check the underside for a burr.'), noReflectiveLaser),
    material('steel', 'Steel / stainless steel', 'metals', ['stainless', 'mild steel', 'ferrous steel'],
      'Ferrous sheet and plate need a process matched to their grade and finish.',
      excludeRouting('General-purpose trim-router tooling in this guide is unsuitable for steel. Select a mill and cutter documented for the alloy.', ['router']),
      noDiode('A blue-diode process is not established here for bare or reflective steel. Wavelength, finish and the specific marking system matter.', ['reflection'])),
    material('machining-wax', 'Machinable wax', 'other', ['tooling wax', 'prototype wax', 'machining wax'],
      'A rigid tooling wax made for milling masters and inspecting toolpaths. Candle wax has different properties.',
      routerOptions(candidate, 'Use machining-grade wax on a rigid carrier. Keep clamping pressure off delicate walls.',
        'Look for smeared detail and chipped corners on the smallest feature.', ['milling', 'cutterShapes']),
      noDiode('Laser heat melts the wax. Use a mechanical process for a dimensioned mould or master.'), noSoftKnife),
    material('glass', 'Clear, uncoated glass', 'other', ['window glass', 'clear glass sheet'],
      'A brittle, transparent mineral sheet. A glass scorer or dedicated glass process uses different tooling from a vinyl blade.',
      excludeRouting('Woodworking end mills and V-bits do not supply a qualified glass process.', ['cutterShapes']),
      noDiode('The blue beam passes through clear glass. This guide covers no direct engraving or through-cut process on this head.', ['diode']), noSoftKnife),
    material('fr4', 'FR-4 fibreglass laminate', 'other', ['pcb', 'fiberglass', 'epoxy glass', 'circuit board'],
      'Glass-fibre fabric and resin, often faced with copper. The resin, conductive layer and abrasive dust need dedicated controls.',
      excludeRouting('This workshop guide has no qualified composite machining setup or dust controls for glass-fibre laminate.', ['milling']),
      noDiode('Exclude this epoxy laminate from laser processing. The resin and any exposed copper both affect the hazard.', ['excluded', 'reflection'])),
    material('foam-board', 'Paper-faced foam board', 'paper', ['foamcore', 'foam core', 'foamboard'],
      'Paper skins surround a foam core. The core and bonding adhesive can differ by brand.',
      excludeRouting('Unidentified foam cores and paper faces call for a sheet-cutting process documented for the stack.'),
      noDiode('Exclude paper-faced foam board. Fires can develop inside the core and spread beyond the visible cut.', ['laserSafety']),
      knifeSheet('Check the full thickness and blade specification. Many mounting boards exceed vinyl blade reach and require a dedicated long-blade cutter.',
        'Check for compressed foam and an unbroken lower paper skin.', conditional)),
    material('chrome-leather', 'Chrome-tanned leather', 'soft', ['chrome tan', 'chromium tanned leather'],
      'A hide tanned with chromium compounds. Dyes, surface finishes and the tanning process are separate material details.', sheerStock,
      noDiode('Exclude chrome-tanned leather from this workshop laser guide. Select a documented, laser-rated vegetable-tanned hide for a laser project.', ['excluded']),
      knifeSheet('Check the holder rating for the thickness. Support the hide without stretching the grain.',
        'Inspect the flesh side for uncut fibres and the face for stretching.', conditional)),
    material('ptfe', 'PTFE sheet', 'plastics', ['polytetrafluoroethylene', 'teflon', 'fluoropolymer'],
      'A fluoropolymer sold as solid sheet, filled stock and adhesive-backed film. This entry covers identified solid sheet.',
      excludeRouting('Use a documented mechanical process for the exact PTFE grade and fillers. This guide has no router setup for them.', ['milling']),
      noDiode('Keep PTFE and other fluoropolymer sheet away from the laser. Laser processing can produce hazardous decomposition products.', ['excluded'])),
    material('unknown', 'Unidentified or reclaimed stock', 'other', ['unknown', 'unlabelled', 'painted scrap', 'treated timber', 'unknown plastic'],
      'Use this entry when the source, treatment, coating, adhesive or substrate is unknown.',
      excludeRouting('Identify the substrate, coating, embedded fasteners and required dust controls before selecting a rotating cutter.', ['dust', 'milling']),
      noDiode('Do not use the laser to identify stock. Identify the complete product from its supplier or select a known material.', ['excluded', 'laserSafety']),
      excludedTool('knife', 'Check the composition, thickness, backing and any embedded hard material before choosing a blade.', ['blades']))
  ];

  const data = { tools, materials, sources };
  globalThis.KerfMaterialsData = data;
  if (typeof module !== 'undefined' && module.exports) module.exports = data;
})();
