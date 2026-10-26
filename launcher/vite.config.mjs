import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `npm run dev` / `npm run build` serve the Tauri window (src-tauri).
export default defineConfig({
  base: './',
  plugins: [react()],
  clearScreen: false,
  server: { port: 5174, strictPort: true, watch: { ignored: ['**/src-tauri/**'] } },
  build: { outDir: 'dist', emptyOutDir: true, target: 'chrome120' },
});
