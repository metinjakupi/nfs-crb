'use strict';
const {app,BrowserWindow,dialog,ipcMain,session} = require('electron');
const path = require('node:path');
const fs = require('node:fs/promises');
const {inspectGame,createLocalServer} = require('./local-server.cjs');
let local,launcher,gameWindow,selection=null,busy=false;
if(!app.requestSingleInstanceLock())app.quit();
else {
  app.on('second-instance',()=>{launcher?.show();launcher?.focus();});
  app.whenReady().then(async()=>{
    const runtime=app.isPackaged?path.join(process.resourcesPath,'runtime'):path.join(__dirname,'runtime');
    const config=path.join(app.getPath('userData'),'launcher.json');
    // One fixed origin preserves browser-backed saves across application restarts.
    local=await createLocalServer({runtime,ui:path.join(__dirname,'ui')});
    const authorized=event=>event.sender===launcher?.webContents&&event.senderFrame?.url===local.origin+'/';
    ipcMain.handle('launcher:state',event=>{if(!authorized(event))throw Error('Invalid launcher request');return {folder:selection?.root||'',storage:app.getPath('userData'),version:app.getVersion(),platform:process.platform};});
    async function select(folder){selection=await inspectGame(folder);local.setGame(selection);await fs.mkdir(path.dirname(config),{recursive:true});await fs.writeFile(config,JSON.stringify({folder:selection.root}));return {folder:selection.root,files:selection.files.size};}
    ipcMain.handle('launcher:choose',async event=>{
      if(!authorized(event)||busy||gameWindow)throw Error('Close the game before changing folders.');
      busy=true;
      try {const picked=await dialog.showOpenDialog(launcher,{title:'Select your Carbon installation',properties:['openDirectory']});if(picked.canceled)return null;return await select(picked.filePaths[0]);}
      finally {busy=false;}
    });
    ipcMain.handle('launcher:play',async event=>{
      if(!authorized(event)||busy)throw Error('The launcher is busy.');
      if(gameWindow){gameWindow.focus();return;}
      if(!selection)throw Error('Select your game folder first.');
      busy=true;
      try {
        // Revalidate before every launch. Never execute NFSC.exe or ASI mods.
        selection=await inspectGame(selection.root);local.setGame(selection);
        await fs.access(path.join(runtime,'nfsc','CarbonRecomp.wasm'));
        const win=new BrowserWindow({title:'Carbon',width:1280,height:800,backgroundColor:'#070c16',webPreferences:{nodeIntegration:false,contextIsolation:true,sandbox:true}});
        gameWindow=win;win.setMenuBarVisibility(false);
        restrict(win);win.on('closed',()=>{gameWindow=null;launcher?.show();});
        await win.loadURL(local.origin+'/play/nfsc/index.html');launcher.hide();
      } finally {busy=false;}
    });
    session.defaultSession.setPermissionRequestHandler((_wc,permission,callback)=>callback(['pointerLock','fullscreen'].includes(permission)));
    function restrict(win){win.webContents.setWindowOpenHandler(()=>({action:'deny'}));win.webContents.on('will-navigate',(event,url)=>{if(new URL(url).origin!==local.origin)event.preventDefault();});}
    launcher=new BrowserWindow({title:'Carbon Local Launcher',width:940,height:650,minWidth:600,minHeight:520,backgroundColor:'#070c16',webPreferences:{preload:path.join(__dirname,'preload.cjs'),nodeIntegration:false,contextIsolation:true,sandbox:true}});
    launcher.setMenuBarVisibility(false);restrict(launcher);
    try {const saved=JSON.parse(await fs.readFile(config,'utf8'));await select(saved.folder);}catch{}
    await launcher.loadURL(local.origin+'/');
  }).catch(error=>{dialog.showErrorBox('Carbon launcher could not start',error.message);app.quit();});
  app.on('window-all-closed',()=>app.quit());
  app.on('before-quit',()=>{local?.close();});
}
