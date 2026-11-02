'use strict';
const api=window.carbonLauncher;
const choose=document.querySelector('#choose'),play=document.querySelector('#play'),status=document.querySelector('#status');
let ready=false;
function message(text,error=false){status.textContent=text;status.classList.toggle('error',error);}
async function action(button,run){choose.disabled=true;play.disabled=true;try{await run();}catch(e){message(e.message,true);}finally{choose.disabled=false;play.disabled=!ready;}}
choose.onclick=()=>action(choose,async()=>{message('Checking game files…');const result=await api.choose();if(result){document.querySelector('#folder').textContent=result.folder;ready=true;message('Game files verified. Ready to play.');}else message(ready?'Ready to play.':'Choose your game folder to get started.');});
play.onclick=()=>action(play,async()=>{message('Starting Carbon…');await api.play();message('Close the game window to return here.');});
if(api)api.state().then(state=>{document.querySelector('#version').textContent=state.version;document.querySelector(".platform").textContent=(state.platform==="darwin"?"macOS":"WINDOWS")+" · PREVIEW";if(state.folder){document.querySelector('#folder').textContent=state.folder;ready=true;play.disabled=false;message('Game folder verified. Ready to play.');}}).catch(e=>message(e.message,true));
else{choose.disabled=true;message('Open this screen through Carbon Local Launcher.',true);}
