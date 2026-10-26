import { useCallback, useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { Download, Loader2, X } from 'lucide-react';
import { call, onBackendExit, onChanged, onDragDrop, onOutput } from './api';
import { Sidebar, TitleBar } from './components/chrome';
import { Empty } from './components/common';
import { Home } from './pages/home';
import { Setup } from './pages/setup';
import { Apps } from './pages/apps';
import { Dig } from './pages/dig';
import '@fontsource-variable/jost';
import './styles.css';

const OUTPUT_LINES = 5000;

function App() {
  const [state, setState] = useState(null);
  const [output, setOutput] = useState([]);
  const [page, setPage] = useState('home');
  const [pending, setPending] = useState(new Set());
  const pendingRef = useRef(new Set());
  const [notice, setNotice] = useState(null);
  const [dragging, setDragging] = useState(false);
  const [checks, setChecks] = useState(null);  // doctor's, once run
  const timer = useRef(null);
  const content = useRef(null);

  const notify = useCallback((text, error = false) => {
    clearTimeout(timer.current); setNotice({ text, error });
    if (!error) timer.current = setTimeout(() => setNotice(null), 4000);
  }, []);
  // Runs one task per key; its key shows as pending until it settles, and its error as a notice.
  const run = useCallback(async (key, task) => {
    if (pendingRef.current.has(key)) return;
    pendingRef.current.add(key); setPending(new Set(pendingRef.current));
    try { return await task(); } catch (error) { notify(String(error?.message || error), true); }
    finally { pendingRef.current.delete(key); setPending(new Set(pendingRef.current)); }
  }, [notify]);

  useEffect(() => {
    let active = true;
    const offs = [
      onChanged(next => { if (active) setState(next); }),
      onOutput(lines => setOutput(old => { const all = old.concat(lines); return all.length > OUTPUT_LINES ? all.slice(-OUTPUT_LINES) : all; })),
      onBackendExit(() => notify("Prism's backend stopped: work\\logs\\launcher-backend.log says why. Restart Prism.", true)),
    ];
    call('state').then(value => { if (active) setState(value); }).catch(error => notify(String(error?.message || error), true));
    call('output').then(lines => { if (active) setOutput(old => old.length ? old : lines); }).catch(() => {});
    return () => { active = false; offs.forEach(off => off()); clearTimeout(timer.current); };
  }, [notify]);

  useEffect(() => { content.current?.scrollTo({ top: 0 }); }, [page]);

  // Ctrl+1..4: the pages, in the sidebar's order.
  useEffect(() => {
    const onKey = e => {
      const target = e.ctrlKey && !e.altKey && ['home', 'setup', 'apps', 'dig'][Number(e.key) - 1];
      if (target) { e.preventDefault(); setPage(target); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  // APKs dropped anywhere on the window install on Horizon.
  useEffect(() => onDragDrop(event => {
    const apks = (event.paths || []).filter(p => /\.apk$/i.test(p));
    if (event.type === 'enter') setDragging(apks.length > 0);  // 'over' has no paths
    else if (event.type !== 'over') setDragging(false);
    if (event.type === 'drop' && apks.length) {
      setPage('apps');
      run('install', async () => { await call('install', apks); notify(`Installing ${apks.length === 1 ? apks[0].split(/[\\/]/).pop() : `${apks.length} APKs`}`); });
    }
  }), [run, notify]);

  const shared = { state, run, pending, notify, setPage, output, checks, setChecks };
  return <div className="app">
    <TitleBar />
    <div className="body">
      <Sidebar page={page} setPage={setPage} state={state} />
      <main className="content" ref={content} aria-label={page}>
        {!state ? <Empty title="Starting Prism" action={<Loader2 className="spin" />} />
          : page === 'home' ? <Home {...shared} />
          : page === 'setup' ? <Setup {...shared} />
          : page === 'apps' ? <Apps {...shared} />
          : <Dig {...shared} />}
      </main>
    </div>
    {dragging && <div className="drop-overlay"><Download /><strong>Drop to install on Horizon</strong></div>}
    {notice && <div role={notice.error ? 'alert' : 'status'} className={`toast ${notice.error ? 'error' : ''}`}>
      <span className="toast-text">{notice.text}</span>
      <button type="button" aria-label="Dismiss notification" onClick={() => setNotice(null)}><X /></button>
    </div>}
  </div>;
}

createRoot(document.getElementById('root')).render(<App />);
