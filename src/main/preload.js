const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('kg', {
  call: (ns, fn, ...args) => ipcRenderer.invoke('call', ns, fn, args),
  onProgress: cb => ipcRenderer.on('progress', (_e, t) => cb(t)),
});
