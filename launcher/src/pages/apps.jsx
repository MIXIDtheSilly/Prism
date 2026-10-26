import { useCallback, useEffect, useRef, useState } from 'react';
import { Download, Loader2, Play, RefreshCw, Search, Square, Trash2 } from 'lucide-react';
import { call } from '../api';
import { Empty, PageHead } from '../components/common';

export function Apps({ state, run, pending, notify, setPage }) {
  const [apps, setApps] = useState(null);
  const [error, setError] = useState('');
  const [system, setSystem] = useState(false);
  const [query, setQuery] = useState('');
  const [armed, setArmed] = useState(null);  // the package an Uninstall click asks to confirm
  const up = state.horizon.stage !== 'stopped' && state.horizon.stage !== 'checking';
  const installing = state.task?.name === 'install' && state.task.running;
  const was = useRef(installing);

  const load = useCallback(() => run('apps', async () => {
    try { setApps(await call('apps', system)); setError(''); }
    catch (e) { setError(String(e?.message || e)); setApps([]); }
  }), [run, system]);
  useEffect(() => { if (up) load(); }, [up, load]);
  useEffect(() => { if (was.current && !installing) load(); was.current = installing; }, [installing, load]);
  useEffect(() => { if (!armed) return; const t = setTimeout(() => setArmed(null), 4000); return () => clearTimeout(t); }, [armed]);

  const act = (app, action) => run(`${action}-${app.package}`, async () => {
    const result = await call('app', app.package, action);
    notify(action === 'launch' ? `Opened ${result}` : action === 'stop' ? `Stopped ${app.package}` : `Uninstalled ${app.package}`);
    load();
  });
  const shown = (apps || []).filter(a => !query || a.package.toLowerCase().includes(query.toLowerCase()));

  return <div className="page">
    <PageHead title="Apps" sub={apps ? `${apps.length} ${system ? 'packages on Horizon' : 'apps installed from outside the system image'}` : 'On the emulator'}>
      <button type="button" className="btn btn-primary" disabled={!up || installing} onClick={() => run('pickApks', () => call('pickApks'))}>
        {installing ? <Loader2 className="spin" /> : <Download />}Install APK</button>
    </PageHead>
    {!up ? <Empty title="Horizon isn't running" action={<button type="button" className="btn btn-outline" onClick={() => setPage('home')}>Start it from Home</button>}>
      Apps install on, and are listed from, the running emulator.</Empty> : <>
      <div className="toolbar">
        <div className="search"><Search /><input type="search" className="field-input" placeholder="Filter by package" value={query} onChange={e => setQuery(e.target.value)} /></div>
        <div className="segmented" role="group">
          <button type="button" aria-pressed={!system} onClick={() => setSystem(false)}>Installed</button>
          <button type="button" aria-pressed={system} onClick={() => setSystem(true)}>Everything</button>
        </div>
        <div className="grow" />
        <span className="muted small">Drop APKs on the window to install them</span>
        <button type="button" className="icon-btn" aria-label="Refresh" title="Refresh" disabled={pending.has('apps')} onClick={load}>
          {pending.has('apps') ? <Loader2 className="spin" /> : <RefreshCw />}</button>
      </div>
      {error && <p className="note">{error}</p>}
      {apps && !shown.length ? <Empty title={query ? 'No match' : 'No apps installed'}>
        {query ? `No package has “${query}”.` : 'Install an APK, or drop one on the window. OpenXR apps launch here or from the library\'s Unknown Sources.'}</Empty>
        : <div className="list">
          {shown.map(app => <div key={app.package} className="list-row">
            <span className={`dot ${app.running ? 'ok' : 'off'}`} title={app.running ? 'Running' : 'Not running'} />
            <div className="list-text">
              <strong className="selectable">{app.package}</strong>
              <span className="mono selectable" title={app.path}>v{app.version} · {app.path}</span>
            </div>
            <div className="row-actions">
              <button type="button" className="btn btn-outline btn-sm" disabled={pending.has(`launch-${app.package}`)} onClick={() => act(app, 'launch')}><Play />Launch</button>
              <button type="button" className="btn btn-ghost btn-sm" disabled={!app.running || pending.has(`stop-${app.package}`)} onClick={() => act(app, 'stop')}><Square />Stop</button>
              {!app.system && <button type="button" className={`btn btn-sm ${armed === app.package ? 'btn-danger' : 'btn-ghost'}`}
                disabled={pending.has(`uninstall-${app.package}`)}
                onClick={() => armed === app.package ? (setArmed(null), act(app, 'uninstall')) : setArmed(app.package)}>
                <Trash2 />{armed === app.package ? 'Confirm' : 'Uninstall'}</button>}
            </div>
          </div>)}
        </div>}
    </>}
  </div>;
}
