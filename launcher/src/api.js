// The window talks to tools/backend.py through src-tauri (main.rs, backend.rs).
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWebview } from '@tauri-apps/api/webview';
import { getCurrentWindow } from '@tauri-apps/api/window';

export const call = (method, ...args) => invoke('call', { method, args });

function subscribe(name, callback) {
  let stop = null, stopped = false;
  listen(name, event => callback(event.payload)).then(unlisten => { if (stopped) unlisten(); else stop = unlisten; });
  return () => { stopped = true; stop?.(); };
}
export const onChanged = callback => subscribe('prism://changed', callback);
export const onOutput = callback => subscribe('prism://output', callback);
export const onLogcat = callback => subscribe('prism://logcat', callback);
export const onBackendExit = callback => subscribe('prism://backend-exit', callback);

// Files dragged onto the window: {type: 'enter' | 'over' | 'drop' | 'leave', paths}.
export function onDragDrop(callback) {
  let stop = null, stopped = false;
  getCurrentWebview().onDragDropEvent(event => callback(event.payload)).then(unlisten => { if (stopped) unlisten(); else stop = unlisten; });
  return () => { stopped = true; stop?.(); };
}

const appWindow = getCurrentWindow();
export const windowControls = {
  minimize: () => appWindow.minimize(),
  toggleMaximize: () => appWindow.toggleMaximize(),
  close: () => appWindow.close(),
};
