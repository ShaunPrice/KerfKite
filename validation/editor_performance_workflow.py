"""Exercise compact undo, SVG drag, indexed project migration and both mesh renderers."""
from playwright.sync_api import expect


def run(page):
    page.evaluate("() => studioReady")
    with page.expect_request(lambda req: req.url.endswith('/api/simulate')) as stock_request:
        sample = page.evaluate("""async () => {
          const outside = [20.12345,20.6789,21.12345,21.6789,-.5,null,-.25];
          const segments = Array(134267).fill(outside);
          segments.push([0,0,8,0,-1,null,-1]);
          const preview = await api('simulate',{segments,width:8,height:8,resolution:16,tool_diameter:1});
          return preview.heights[0];
        }""")
    assert stock_request.value.headers['content-type']=='application/vnd.kerfkite.stock-v1'
    assert sample == [-1]*16
    loc=page.locator
    page.get_by_role("tab",name="Design",exact=True).click()
    loc("#rect").click()
    loc("#ellipse").click()
    page.evaluate("""() => {
      const object=objects[selected];
      object.depths=object.paths.map(path=>path.map((_,i)=>-i/100));
      object.powers=object.paths.map(path=>path.map((_,i)=>i/200));
      window.recordHistory();
    }""")
    original=page.evaluate("() => CNCProjects.document().objects")
    page.get_by_role("tab",name="Objects",exact=True).click()
    loc("#remove").click()
    page.get_by_role("tab",name="Design",exact=True).click()
    page.evaluate("() => document.getElementById('undo').click()")
    assert page.evaluate("() => CNCProjects.document().objects")==original
    page.evaluate("() => document.getElementById('redo').click()")
    assert len(page.evaluate("() => CNCProjects.document().objects"))==1

    # Pointer moves keep the drawing DOM stable until the final geometric commit.
    page.evaluate("KerfKiteViews.apply('fit-drawing',[])")
    point=page.evaluate("""() => {
      const p=objects[0].paths[0],h=+$('height').value;
      const pt=new DOMPoint(p[0][0]+2,h-p[0][1]-2).matrixTransform($('canvas').getScreenCTM());
      return [pt.x,pt.y];
    }""")
    page.mouse.move(*point)
    page.mouse.down()
    page.evaluate("window.dragPath=$('drawing').firstChild;window.dragList=$('objects').firstChild")
    page.mouse.move(point[0]+10,point[1]+4,steps=3)
    page.wait_for_function("window.dragPath.hasAttribute('transform')")
    assert page.evaluate("window.dragPath===$('drawing').firstChild && window.dragList===$('objects').firstChild")
    page.keyboard.press('Escape')
    page.mouse.up()
    assert page.evaluate("() => CNCProjects.document().objects")==original[:1]

    # A closed v1 browser library populated with two accounts, a revision and a
    # draft exercises the migration and prevents geometry reads while searching.
    url=page.url
    doc=page.evaluate("() => CNCProjects.document()")
    doc["metadata"]={"id":"v1","name":"Migrated plate"}
    page.goto(url.rstrip('/')+'/favicon.ico')
    page.evaluate("""async d=>{
      const wait=q=>new Promise((res,rej)=>{q.onsuccess=()=>res(q.result);q.onerror=()=>rej(q.error);});
      await wait(indexedDB.deleteDatabase('pi-cnc-project-library'));
      const req=indexedDB.open('pi-cnc-project-library',1);
      req.onupgradeneeded=()=>req.result.createObjectStore('projects',{keyPath:'key'});
      const db=await wait(req),tx=db.transaction('projects','readwrite'),s=tx.objectStore('projects');
      const previous=structuredClone(d);previous.metadata.name='First plate';
      const base={key:'local:v1',namespace:'local',document:d,saved:'2026-01-02',revisions:[{saved:'2026-01-01',document:previous}]};
      s.put(base);s.put({...base,key:'local:@draft',draft:true});
      const legacy=structuredClone(d);delete legacy.metadata;
      s.put({key:'local:legacy',namespace:'local',document:legacy,revisions:{malformed:true}});
      s.put({key:'local:damaged',namespace:'local',document:{format:'invalid',metadata:{name:'Damaged project'}},
        revisions:[null, {saved:null,document:previous}, 'invalid entry']});
      s.put({...base,key:'account:example:v1',namespace:'account:example'});
      await new Promise((res,rej)=>{tx.oncomplete=res;tx.onabort=()=>rej(tx.error);});db.close();
    }""",doc)
    page.goto(url)
    page.get_by_role("tab",name="Projects",exact=True).click()
    expect(loc('#projectList p')).to_have_count(3)
    expect(loc('#projectList')).to_contain_text('Migrated plate')
    read_legacy="""async () => {
      const req=indexedDB.open('pi-cnc-project-library',2);
      const db=await new Promise((resolve,reject)=>{req.onsuccess=()=>resolve(req.result);req.onerror=()=>reject(req.error);});
      const read=db.transaction('projects').objectStore('projects').get('local:legacy');
      try{return await new Promise((resolve,reject)=>{read.onsuccess=()=>resolve(read.result);read.onerror=()=>reject(read.error);});}
      finally{db.close();}
    }"""
    baseline=page.evaluate(read_legacy)
    assert 'metadata' not in baseline['document'] and baseline['revisions']=={'malformed':True}
    damaged=loc('#projectList p').filter(has_text='Damaged project')
    damaged.get_by_role('button',name='Open').click()
    expect(loc('#message')).to_contain_text('Invalid project file')
    damaged.locator('select').select_option('0')
    expect(loc('#projectName')).to_have_value('First plate')
    damaged.get_by_role('button',name='Delete').click()
    expect(damaged).to_have_count(0)
    legacy=loc('#projectList p').filter(has_text='Untitled project')
    legacy.get_by_role('button',name='Open').click()
    expect(loc('#projectName')).to_have_value('Untitled project')
    assert page.evaluate(read_legacy)==baseline
    assert page.evaluate('CNCProjects.document().metadata.id')=='legacy'
    loc('#projectName').fill('Restored legacy')
    loc('#projectSaveLibrary').click()
    repaired=loc('#projectList p').filter(has_text='Restored legacy')
    expect(repaired).to_have_count(1)
    expect(repaired.locator('select option')).to_have_count(2)
    restored=page.evaluate(read_legacy)
    assert restored['revisions'][0]['document']==baseline['document']
    repaired.get_by_role('button',name='Delete').click()
    expect(loc('#projectList p')).to_have_count(1)
    page.evaluate("""() => {
      window.projectReads=0;
      for(const name of ['get','getAll','openCursor']){
        const method=IDBObjectStore.prototype[name];
        IDBObjectStore.prototype[name]=function(...args){
          if(this.name==='projects')window.projectReads++;
          return method.apply(this,args);
        };
      }
    }""")
    loc('#projectSearch').fill('no matching name')
    expect(loc('#projectList')).to_have_text('No matching saved projects.')
    loc('#projectSearch').fill('migrated')
    expect(loc('#projectList p')).to_have_count(1)
    assert page.evaluate('window.projectReads')==0
    loc('#projectList select').select_option('0')
    expect(loc('#projectName')).to_have_value('First plate')
    assert page.evaluate('window.projectReads')==1
    loc('#projectSaveLibrary').click()
    expect(loc('#projectList')).to_have_text('No matching saved projects.')
    loc('#projectSearch').fill('')
    expect(loc('#projectList')).to_contain_text('First plate')
    loc('#projectList').get_by_role('button',name='Delete').click()
    expect(loc('#projectList')).to_have_text('No matching saved projects.')
    loc('#projectRecover').click()
    expect(loc('#projectName')).to_have_value('Migrated plate')

    page.get_by_role("tab",name="3D CAD",exact=True).click()
    loc('#solid-add').click()
    expect(loc('#solid-status')).to_have_text('Solid ready.')
    live=page.evaluate("""() => {
      CNCSolidCamera.apply('iso');
      const target=$('solid-canvas'),surface=target.parentElement.querySelector('.mesh-surface');
      if(target.dataset.meshRenderer==='webgl2'){
        if(!surface||surface.hidden)throw Error('GPU viewport missing visible surface');
        const pixel=new Uint8Array(4),gl=surface.getContext('webgl2');
        gl.readPixels(surface.width>>1,surface.height>>1,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);
        if(pixel[3]!==255)throw Error('Indexed viewport centre was blank');
        if(target.getBoundingClientRect().height!==surface.getBoundingClientRect().height)throw Error('Misaligned viewport');
      }else if(!target.getContext('2d').getImageData(target.width>>1,target.height>>1,1,1).data[3]){
        throw Error('Canvas fallback was blank');
      }
      return target.dataset.meshRenderer;
    }""")

    result=page.evaluate("""() => {
      const make=()=>{const el=document.createElement('canvas');el.style='width:180px;height:160px;';
        document.body.append(el);return el;};
      const gpu=make(),fallback=make(),mesh={vertices:[[0,0,0],[4,0,0],[0,5,0],[0,0,2]],
        triangles:[[0,2,1],[0,1,3],[0,3,2],[1,2,3]],bounds:[0,0,0,4,5,2]};
      const camera={yaw:-Math.PI/4,pitch:.6};
      let uploads=0,glType=window.WebGL2RenderingContext;
      const original=glType?.prototype.bufferData;
      if(glType)glType.prototype.bufferData=function(...args){uploads++;return original.apply(this,args);};
      const get=HTMLCanvasElement.prototype.getContext;
      try{
        KerfKiteViews.paintMesh(gpu,mesh,camera);const first=uploads;
        KerfKiteViews.paintMesh(gpu,mesh,{...camera,yaw:.8});
        KerfKiteViews.paintMesh(gpu,mesh,camera);
        if(gpu.dataset.meshRenderer==='webgl2' && (first!==2||uploads!==first))throw Error('Camera move uploaded geometry');
        HTMLCanvasElement.prototype.getContext=function(type,...args){return type==='webgl2'?null:get.call(this,type,...args);};
        KerfKiteViews.paintMesh(fallback,mesh,camera);
        if(fallback.dataset.meshRenderer!=='canvas2d')throw Error('Missing Canvas fallback');
        function pixels(c){return c.getContext('2d').getImageData(0,0,c.width,c.height).data;}
        const a=pixels(gpu),b=pixels(fallback);let common=0,covered=0;
        for(let i=3;i<a.length;i+=4){if(a[i]>127||b[i]>127)covered++;if(a[i]>127&&b[i]>127)common++;}
        if(covered<300||common/covered<.94)throw Error('GPU and Canvas silhouettes differ: '+common+'/'+covered);
        return {renderer:gpu.dataset.meshRenderer,uploads,overlap:common/covered};
      }finally{
        if(glType)glType.prototype.bufferData=original;
        HTMLCanvasElement.prototype.getContext=get;
        KerfKiteMeshRenderer.clear(gpu);KerfKiteMeshRenderer.clear(fallback);gpu.remove();fallback.remove();
      }
    }""")
    print('PASS geometry-preserving undo/redo, stable drag frames and cancel, v1 migration with revision '
          f'and draft, metadata-only search, {live} part viewport, cached geometry and fallback {result}')
