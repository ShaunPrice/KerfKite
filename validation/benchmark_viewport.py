"""Measure cached mesh orbit frame intervals in the filesystem editor on this device."""
import argparse
import json
from pathlib import Path

from playwright.sync_api import sync_playwright


def measure(page):
    return page.evaluate("""async () => {
      const make=()=>{
        const wrap=document.createElement('div'),target=document.createElement('canvas');
        wrap.className='mesh-viewport';
        wrap.style.cssText='position:fixed;top:0;left:0;width:640px;z-index:10000;background:white;';
        target.style.cssText='width:640px;height:360px';wrap.append(target);document.body.append(wrap);
        return target;
      };
      const targets=[make(),make()],n=96;
      const mesh={vertices:[],triangles:[],bounds:[0,0,0,n,n,14]};
      for(let y=0;y<=n;y++)for(let x=0;x<=n;x++)mesh.vertices.push([x,y,7+7*Math.sin(x/8)*Math.cos(y/12)]);
      for(let y=0;y<n;y++)for(let x=0;x<n;x++){
        const a=y*(n+1)+x;mesh.triangles.push([a,a+1,a+n+1],[a+n+1,a+1,a+n+2]);
      }
      const paint=(target,yaw)=>KerfKiteViews.paintMesh(target,mesh,{yaw,pitch:.6});
      paint(targets[0],0);
      const get=HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext=function(type,...args){return type==='webgl2'?null:get.call(this,type,...args);};
      try{paint(targets[1],0);}finally{HTMLCanvasElement.prototype.getContext=get;}
      const gl=targets[0].parentNode.querySelector('.mesh-surface')?.getContext('webgl2');
      const info=gl?.getExtension('WEBGL_debug_renderer_info');
      const report={triangles:mesh.triangles.length,css_pixels:[640,360],pixel_ratio:devicePixelRatio,
        driver:info?gl.getParameter(info.UNMASKED_RENDERER_WEBGL):null,results:[]};
      const tick=()=>new Promise(resolve=>requestAnimationFrame(resolve));
      for(const [i,target] of targets.entries()){
        targets[1-i].parentElement.hidden=true;target.parentElement.hidden=false;
        const intervals=[];let prior=await tick();
        for(let frame=0;frame<35;frame++){
          paint(target,frame/30);const now=await tick();
          if(frame>=3)intervals.push(now-prior);
          prior=now;
        }
        intervals.sort((a,b)=>a-b);
        report.results.push({path:i?'forced fallback':'automatic',renderer:target.dataset.meshRenderer,
          frames:intervals.length,median_frame_ms:(intervals[15]+intervals[16])/2});
      }
      for(const target of targets){KerfKiteMeshRenderer.clear(target);target.parentElement.remove();}
      return report;
    }""")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser',help='Chromium executable, e.g. the installed Chrome or Chromium browser')
    args=parser.parse_args()
    with sync_playwright() as pw:
        browser=pw.chromium.launch(executable_path=args.browser)
        try:
            page=browser.new_page(viewport={'width':1440,'height':980})
            page.goto((Path(__file__).resolve().parents[1]/'studio/static/index.html').as_uri())
            page.wait_for_function('window.KerfKiteViews')
            print(json.dumps(measure(page),indent=2))
        finally:
            browser.close()


if __name__=='__main__':
    main()
