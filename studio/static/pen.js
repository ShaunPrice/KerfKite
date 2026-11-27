/* Pen drawing: the controls of the pen head on the CAM tab. The fields themselves are in index.html, so that
   they are saved and restored with the other settings. The pen's program is made by the core (mode "pen");
   offline-cam.js makes the same for line operations. Lettering for a pen is the Text tool's single-line face
   (stroke-font.js). */
(function(root){
'use strict';
if(typeof document==='undefined'||!document.querySelector)return;
const $=id=>document.getElementById(id);
if(!$('penSettings'))return;
const task=root.task||(fn=>fn);
const changed=id=>$(id).dispatchEvent(new Event('input',{bubbles:true}));

// "Times over each line" is the job's passes, which otherwise show only for a laser.
$('penPasses').oninput=()=>{const n=Math.round(Number($('penPasses').value));if(n>=1&&n<=100){$('passes').value=n;changed('passes');}};
$('mode').addEventListener('change',()=>{if($('mode').value==='pen')$('penPasses').value=$('passes').value;});
// Typical settings for a spring-loaded pen: pressed 1 mm, lifted 3 mm, drawn briskly. For a servo the S value is
// left alone: it is the holder's own.
$('penDefaults').onclick=task(()=>{
 const set=(id,value)=>{$(id).value=value;};
 set('operation','line');set('depth',1);set('safe',3);set('plunge',600);set('feed',2000);set('passes',1);set('penPasses',1);
 changed('feed');
 message('Pen settings applied: pressed 1 mm below Z zero, lifted 3 mm, 2000 mm/min. Set Z zero where the pen just touches the paper.');
});

})(typeof globalThis!=='undefined'?globalThis:this);
