/* Display-only indexed mesh buffers. CAM, CAD rebuilds and exports retain the
 * original numeric coordinates. In the editor the GPU draws directly behind
 * the interaction canvas; the 2D overlay draws axes, labels and fallback faces. */
(() => {
  'use strict';
  const renderers=new WeakMap();
  const vertex=`#version 300 es
    in vec3 position;
    uniform vec4 rotation;
    uniform vec3 scale;
    uniform vec2 pan;
    out vec3 rotated;
    void main(){
      float depth=dot(position.xy,rotation.yx);
      rotated=vec3(rotation.x*position.x-rotation.y*position.y,
                   rotation.w*depth+rotation.z*position.z,
                   -rotation.z*depth+rotation.w*position.z);
      gl_Position=vec4(rotated*scale+vec3(pan,0.),1.);
    }`;
  const fragment=`#version 300 es
    precision highp float;
    in vec3 rotated;
    out vec4 colour;
    void main(){
      vec3 normal=cross(dFdx(rotated),dFdy(rotated));
      normal*=gl_FrontFacing?1.:-1.;
      float light=max(0.,dot(normal,vec3(-.32,.56,.72))/max(1e-20,length(normal)));
      float l=.43+light*.33, c=(1.-abs(2.*l-1.))*.40;
      colour=vec4(vec3(0.,.86667,1.)*c+l-.5*c,1.);
    }`;
  function programFor(gl){
    const program=gl.createProgram();
    if(!program)return null;
    for(const [kind,source] of [[gl.VERTEX_SHADER,vertex],[gl.FRAGMENT_SHADER,fragment]]){
      const shader=gl.createShader(kind);
      if(!shader){gl.deleteProgram(program);return null;}
      gl.shaderSource(shader,source);gl.compileShader(shader);
      if(!gl.getShaderParameter(shader,gl.COMPILE_STATUS)){
        gl.deleteShader(shader);gl.deleteProgram(program);return null;
      }
      gl.attachShader(program,shader);gl.deleteShader(shader);
    }
    gl.linkProgram(program);
    if(!gl.getProgramParameter(program,gl.LINK_STATUS)){gl.deleteProgram(program);return null;}
    return program;
  }
  function create(target){
    const surface=document.createElement('canvas');
    const gl=surface.getContext('webgl2',{alpha:true,antialias:true,depth:true,stencil:false,failIfMajorPerformanceCaveat:true});
    if(!gl)return null;
    const layered=target.parentElement?.classList.contains('mesh-viewport');
    if(layered){
      surface.className='mesh-surface';surface.hidden=true;surface.setAttribute('aria-hidden','true');
      target.before(surface);
    }
    const state={gl,surface,mesh:null,layered};
    function setup(){
      state.program=programFor(gl);
      if(!state.program)return;
      state.rotation=gl.getUniformLocation(state.program,'rotation');
      state.scale=gl.getUniformLocation(state.program,'scale');
      state.pan=gl.getUniformLocation(state.program,'pan');
      state.position=gl.getAttribLocation(state.program,'position');
      gl.enable(gl.DEPTH_TEST);
      gl.enableVertexAttribArray(state.position);
    }
    surface.addEventListener('webglcontextlost',event=>{
      event.preventDefault();surface.hidden=true;state.mesh=null;state.program=null;
      state.vertices=null;state.indices=null;
      target.dispatchEvent(new Event('mesh-renderer-changed'));
    });
    surface.addEventListener('webglcontextrestored',()=>{
      setup();target.dispatchEvent(new Event('mesh-renderer-changed'));
    });
    setup();
    return state;
  }
  function release(state){
    if(!state)return;
    if(state.layered)state.surface.hidden=true;
    state.gl.deleteBuffer(state.vertices||null);state.gl.deleteBuffer(state.indices||null);
    state.vertices=null;state.indices=null;state.mesh=null;
  }
  function upload(state,mesh,centre){
    release(state);
    const {gl}=state;
    const points=new Float32Array(mesh.vertices.length*3);
    for(let i=0;i<mesh.vertices.length;i++){
      const v=mesh.vertices[i];
      for(let k=0;k<3;k++)points[3*i+k]=v[k]-centre[k];
    }
    const compact=mesh.vertices.length<=65536;
    const indices=new (compact?Uint16Array:Uint32Array)(mesh.triangles.length*3);
    for(let i=0;i<mesh.triangles.length;i++)indices.set(mesh.triangles[i],3*i);
    state.vertices=gl.createBuffer();state.indices=gl.createBuffer();
    if(!state.vertices||!state.indices){release(state);return false;}
    gl.bindBuffer(gl.ARRAY_BUFFER,state.vertices);gl.bufferData(gl.ARRAY_BUFFER,points,gl.STATIC_DRAW);
    gl.vertexAttribPointer(state.position,3,gl.FLOAT,false,0,0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,state.indices);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,indices,gl.STATIC_DRAW);
    if(gl.getError()!==gl.NO_ERROR){release(state);return false;}
    state.mesh=mesh;state.indexType=compact?gl.UNSIGNED_SHORT:gl.UNSIGNED_INT;
    return true;
  }
  function draw(target,ctx,mesh,centre,rotation,scale,pan){
    if(!renderers.has(target))renderers.set(target,create(target));
    const state=renderers.get(target);
    if(!state?.program)return false;
    const {surface,gl}=state;
    if(surface.width!==target.width)surface.width=target.width;
    if(surface.height!==target.height)surface.height=target.height;
    if(state.mesh!==mesh&&!upload(state,mesh,centre))return false;
    gl.viewport(0,0,surface.width,surface.height);
    gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    gl.useProgram(state.program);
    gl.uniform4fv(state.rotation,rotation);gl.uniform3fv(state.scale,scale);gl.uniform2fv(state.pan,pan);
    gl.drawElements(gl.TRIANGLES,mesh.triangles.length*3,state.indexType,0);
    if(gl.isContextLost())return false;
    if(state.layered)surface.hidden=false;
    else ctx.drawImage(surface,0,0);
    return true;
  }
  window.KerfKiteMeshRenderer={draw,clear(target){release(renderers.get(target));}};
})();
