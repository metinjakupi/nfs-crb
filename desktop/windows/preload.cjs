'use strict';
const {contextBridge,ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('carbonLauncher',{
  state:()=>ipcRenderer.invoke('launcher:state'),
  choose:()=>ipcRenderer.invoke('launcher:choose'),
  play:()=>ipcRenderer.invoke('launcher:play')
});
