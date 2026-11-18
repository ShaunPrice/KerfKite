(() => {
  'use strict';

  const byId = id => document.getElementById(id);
  const data = window.KerfMaterialsData;
  if (!data?.materials?.length || !data.tools?.length) {
    byId('guide-error').hidden = false;
    return;
  }

  const toolInput = byId('guide-tool');
  const operationInput = byId('guide-operation');
  const groupInput = byId('guide-group');
  const searchInput = byId('guide-search');
  const statusInput = byId('guide-status');
  const materialInput = byId('guide-material');
  const compareInput = byId('guide-compare');
  const statusLabels = {candidate: 'Sample candidate', conditional: 'Conditional', avoid: 'Avoid'};
  const resultLabels = {
    candidate: 'Candidate for a sample',
    conditional: 'Check the requirements first',
    avoid: 'Avoid this combination'
  };
  const materials = [...data.materials].sort((a, b) => a.name.localeCompare(b.name, 'en'));
  const materialMap = new Map(materials.map(material => [material.id, material]));
  const normalize = text => text.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').trim();
  const searchable = new Map(materials.map(material => [
    material.id, normalize([material.name, material.group, ...material.aliases].join(' '))
  ]));
  const initial = new URLSearchParams(location.search);
  let preferredMaterial = initial.get('material');
  let notebookSelection = '';
  let comparisonId = '';

  function option(value, label) {
    return new Option(label, value);
  }

  function element(tag, text, className) {
    const item = document.createElement(tag);
    item.textContent = text;
    if (className) item.className = className;
    return item;
  }

  function currentTool() {
    return data.tools.find(tool => tool.id === toolInput.value);
  }

  function currentOperation(tool) {
    return tool.operations.find(operation => operation.id === operationInput.value);
  }

  function assessment(material, tool, operation) {
    const entry = material.tools[tool.id];
    const detail = entry.operations[operation.id];
    return {entry, status: detail?.status ?? entry.status, advice: detail?.advice || entry.summary};
  }

  function setOperations(preferred) {
    const operations = currentTool().operations;
    operationInput.replaceChildren(...operations.map(item => option(item.id, item.label)));
    if (operations.some(item => item.id === preferred)) operationInput.value = preferred;
  }

  function references(ids) {
    return ids.map(id => {
      const source = data.sources[id];
      const link = element('a', source.title);
      link.href = source.url;
      link.target = '_blank';
      link.rel = 'noopener noreferrer';
      const li = document.createElement('li');
      li.append(link);
      return li;
    });
  }

  function comparisonCard(material, tool, operation) {
    const {entry, status, advice} = assessment(material, tool, operation);
    const card = document.createElement('div');
    card.className = 'compare-card';
    card.dataset.status = status;
    card.append(element('h4', material.name), element('p', statusLabels[status], 'rating'), element('p', advice));
    if (status !== 'avoid' && entry.summary !== advice) card.append(element('p', entry.summary));
    return card;
  }

  function showComparison() {
    const current = materialMap.get(materialInput.value);
    const other = materialMap.get(compareInput.value);
    const region = byId('guide-comparison');
    const tool = currentTool();
    const operation = currentOperation(tool);
    const show = current && other && current !== other;
    region.hidden = !show;
    if (show) {
      region.dataset.status = assessment(other, tool, operation).status;
      region.replaceChildren(comparisonCard(current, tool, operation), comparisonCard(other, tool, operation));
    } else {
      delete region.dataset.status;
      region.replaceChildren();
    }
  }

  // These cross-sections show the geometry of the operation, independent of stock and settings.
  const laserHead = 'M86 10H114V22L106 29H94L86 22Z';
  const crossSections = {
    flat: {
      profile: {
        stock: 'M8 48H91V78H8Z M109 48H192V78H109Z', edge: 'M91 48V78 M109 48V78',
        cutter: 'M92 12H108V77H92Z', caption: 'A flat end mill follows the outline. A through profile separates the part from the surrounding stock.'
      },
      pocket: {
        stock: 'M8 48H64V65H136V48H192V78H8Z', edge: 'M64 48V65H136V48',
        cutter: 'M119 12H135V64H119Z', caption: 'The cutter clears an enclosed area and leaves stock beneath a level floor.'
      },
      engrave: {
        stock: 'M8 48H92V55H108V48H192V78H8Z', edge: 'M92 48V55H108V48',
        cutter: 'M93 12H107V54H93Z', caption: 'A shallow pass follows a line and leaves a flat-bottomed groove about one cutter wide.'
      },
      relief: {
        stock: 'M8 48H50V52H65V57H80V62H95V66H110V62H125V57H140V52H155V48H192V78H8Z',
        edge: 'M50 48V52H65V57H80V62H95V66H110V62H125V57H140V52H155V48',
        cutter: 'M96 12H109V65H96Z', caption: 'Adjacent passes at different depths approximate a curved surface. A flat cutter can leave visible steps.'
      }
    },
    ball: {
      relief: {
        stock: 'M8 48H46C65 48 75 70 94 65Q103 76 113 65C131 57 140 47 157 48H192V78H8Z',
        edge: 'M46 48C65 48 75 70 94 65Q103 76 113 65C131 57 140 47 157 48',
        cutter: 'M94 12H112V61A9 9 0 0 1 94 61Z', caption: 'The rounded tool tip follows the relief. Adjacent passes can leave small ridges between them.'
      }
    },
    v: {
      engrave: {
        stock: 'M8 48H86L100 65L114 48H192V78H8Z', edge: 'M86 48L100 65L114 48',
        cutter: 'M88 12H112V38H122L100 64L78 38H88Z', caption: 'The V-shaped tip opens a wider groove as it travels deeper into the stock.'
      }
    },
    drill: {
      drill: {
        stock: 'M8 46H90V61L100 67L110 61V46H192V78H8Z', edge: 'M90 46V61L100 67L110 61V46',
        cutter: 'M90 9H110V61L100 67L90 61Z', caption: 'The tip reaches the programmed depth. The full-diameter bore ends higher up the conical point.'
      },
      peck: {
        stock: 'M8 46H90V61L100 67L110 61V46H192V78H8Z', edge: 'M90 46V61L100 67L110 61V46',
        cutter: 'M90 8H110V27L100 33L90 27Z', caption: 'Between increments the drill returns to the clearance plane, then feeds to the next depth.'
      }
    },
    diode: {
      engrave: {
        stock: 'M8 48H192V78H8Z', edge: 'M78 48H111', cutter: laserHead, beam: 'M100 29V47',
        caption: 'The beam marks the exposed face while the body of the sheet stays intact.'
      },
      cut: {
        stock: 'M8 48H98L99 78H8Z M102 48H192V78H101Z', edge: 'M98 48L99 78 M102 48L101 78',
        cutter: laserHead, beam: 'M100 29V77', caption: 'A narrow kerf passes through the sheet along the outline.'
      }
    },
    knife: {
      kiss: {
        stock: 'M8 48H99V57H8Z M101 48H192V57H101Z', edge: 'M100 48V57',
        cutter: 'M95 12H110V35L100 56L95 38Z', backing: 'M8 58H192V67H8Z',
        caption: 'The blade cuts the face film and adhesive. The liner underneath stays in one piece.'
      },
      through: {
        stock: 'M8 48H99V65H8Z M101 48H192V65H101Z', edge: 'M100 48V65',
        cutter: 'M95 12H110V40L100 65L95 43Z', backing: 'M8 67H192V76H8Z',
        caption: 'The blade reaches the bottom of the stock. A sacrificial cutting surface supports it underneath.'
      }
    }
  };

  function showCrossSection(tool, operation, status) {
    const drawing = crossSections[tool.id]?.[operation.id];
    const figure = byId('guide-schematic');
    figure.hidden = !drawing || status === 'avoid';
    if (figure.hidden) return;
    byId('schematic-stock').setAttribute('d', drawing.stock);
    byId('schematic-edge').setAttribute('d', drawing.edge);
    byId('schematic-tool').setAttribute('d', drawing.cutter);
    byId('schematic-beam').setAttribute('d', drawing.beam || '');
    byId('schematic-backing').setAttribute('d', drawing.backing || '');
    byId('schematic-caption').textContent = drawing.caption;
  }

  function reflectSelection(material) {
    const url = new URL(location.href);
    url.searchParams.set('tool', toolInput.value);
    url.searchParams.set('operation', operationInput.value);
    url.searchParams.set('material', material.id);
    if (url.href !== location.href) {
      try {
        history.replaceState(null, '', url);
      } catch {
        // Some browsers restrict URL updates on directly opened local files.
      }
    }
  }

  function showMaterial() {
    const material = materialMap.get(materialInput.value);
    if (!material) return;
    const tool = currentTool();
    const operation = currentOperation(tool);
    const {entry, status, advice} = assessment(material, tool, operation);

    byId('detail-family').textContent = material.group;
    byId('guide-title').textContent = material.name;
    byId('detail-description').textContent = material.description;
    byId('guide-verdict').dataset.status = status;
    byId('verdict-context').textContent = tool.label + ' · ' + operation.label;
    byId('verdict-title').textContent = resultLabels[status];
    byId('guide-advice').textContent = advice;
    byId('guide-summary').textContent = entry.summary;
    byId('guide-summary').hidden = status === 'avoid' || advice === entry.summary;
    // A tool can work with this stock for a different operation. Hide that operation's setup.
    byId('guide-checklists').hidden = status === 'avoid' && entry.status !== 'avoid';
    showCrossSection(tool, operation, status);
    byId('guide-setup').replaceChildren(...entry.setup.map(text => element('li', text)));
    byId('guide-inspect').replaceChildren(...entry.inspect.map(text => element('li', text)));
    byId('guide-sources').replaceChildren(...references(entry.sources));
    if (comparisonId === material.id) comparisonId = '';
    compareInput.replaceChildren(
      option('', 'Choose a material to compare'),
      ...materials.filter(item => item !== material).map(item => option(item.id, item.name))
    );
    compareInput.value = comparisonId;
    showComparison();
    const selection = [material.id, tool.id, operation.id].join('/');
    if (selection !== notebookSelection) {
      window.KerfMaterialsNotebook?.show(byId('sample-notebook'), {material, tool, operation, status});
      notebookSelection = selection;
    }
    reflectSelection(material);
  }

  function filterMaterials() {
    const terms = normalize(searchInput.value).split(/\s+/).filter(Boolean);
    const group = groupInput.value;
    const status = statusInput.value;
    const tool = currentTool();
    const operation = currentOperation(tool);
    const matches = materials.filter(material =>
      (group === 'all' || group === material.group) &&
      (status === 'all' || assessment(material, tool, operation).status === status) &&
      terms.every(term => searchable.get(material.id).includes(term))
    );
    const previous = materialInput.value;
    materialInput.replaceChildren(...matches.map(material => option(material.id, material.name)));
    const selected = matches.find(item => item.id === preferredMaterial) ||
                     matches.find(item => item.id === previous) || matches[0];
    materialInput.disabled = !selected;
    if (selected) materialInput.value = selected.id;
    byId('guide-reset').disabled = !searchInput.value && group === 'all' && status === 'all';
    byId('guide-count').textContent = matches.length + ' of ' + materials.length;
    byId('guide-empty').hidden = Boolean(selected);
    byId('guide-detail').hidden = !selected;
    showMaterial();
  }

  function resetFilters() {
    searchInput.value = '';
    groupInput.value = 'all';
    statusInput.value = 'all';
    filterMaterials();
    searchInput.focus();
  }

  toolInput.replaceChildren(...data.tools.map(tool => option(tool.id, tool.label)));
  if (data.tools.some(tool => tool.id === initial.get('tool'))) toolInput.value = initial.get('tool');
  setOperations(initial.get('operation'));
  if (!materialMap.has(preferredMaterial)) {
    const tool = currentTool();
    const operation = currentOperation(tool);
    preferredMaterial = data.materials.find(material => assessment(material, tool, operation).status === 'candidate')?.id;
  }
  const groups = [...new Set(materials.map(material => material.group))].sort();
  groupInput.replaceChildren(option('all', 'All families'), ...groups.map(group => option(group, group)));

  toolInput.addEventListener('change', () => {
    setOperations(operationInput.value);
    filterMaterials();
  });
  for (const input of [operationInput, statusInput, groupInput]) input.addEventListener('change', filterMaterials);
  searchInput.addEventListener('input', filterMaterials);
  materialInput.addEventListener('change', () => {
    preferredMaterial = materialInput.value;
    showMaterial();
  });
  compareInput.addEventListener('change', () => {
    comparisonId = compareInput.value;
    showComparison();
  });
  byId('guide-reset').addEventListener('click', resetFilters);
  byId('guide-empty-reset').addEventListener('click', resetFilters);

  const narrow = matchMedia('(max-width: 52rem)');
  function sizeMaterialList() {
    materialInput.size = narrow.matches ? 1 : 8;
  }
  narrow.addEventListener('change', sizeMaterialList);
  sizeMaterialList();
  byId('guide-body').hidden = false;
  filterMaterials();
})();
