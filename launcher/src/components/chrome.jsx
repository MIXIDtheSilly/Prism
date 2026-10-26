import { House, LayoutGrid, Minus, Search, Square, Wrench, X } from 'lucide-react';
import { windowControls } from '../api';
import { STAGES, stageTone } from './common';
import { LogoMark } from './logo';

export function TitleBar() {
  return <div className="titlebar">
    <div className="titlebar-drag" data-tauri-drag-region><LogoMark /><span data-tauri-drag-region>Prism</span></div>
    <div className="window-controls">
      <button type="button" aria-label="Minimize" onClick={windowControls.minimize}><Minus /></button>
      <button type="button" aria-label="Maximize" onClick={windowControls.toggleMaximize}><Square style={{ width: 13 }} /></button>
      <button type="button" aria-label="Close" className="close" onClick={windowControls.close}><X /></button>
    </div>
  </div>;
}

const pages = [
  ['home', 'Home', House],
  ['setup', 'Setup', Wrench],
  ['apps', 'Apps', LayoutGrid],
  ['dig', 'Dig', Search],
];

export function Sidebar({ page, setPage, state }) {
  const horizon = state?.horizon;
  const todo = state?.steps?.filter(s => s.current === false).length || 0;
  const task = state?.task?.running && state.task;
  return <aside className="sidebar">
    <button type="button" className="brand" onClick={() => setPage('home')} aria-label="Prism home">
      <LogoMark /><span>Prism</span>
    </button>
    <nav className="nav" aria-label="Main navigation">
      {pages.map(([id, label, Icon]) => <button key={id} type="button" className="nav-item"
        aria-current={page === id ? 'page' : undefined} onClick={() => setPage(id)}>
        <Icon />{label}
        {id === 'setup' && todo > 0 && <span className="nav-badge" title={`${todo} step(s) to do`}>{todo}</span>}
      </button>)}
    </nav>
    <div className="sidebar-spacer" />
    {horizon && <button type="button" className="status-card" onClick={() => setPage('home')}>
      <span className={`dot ${stageTone(horizon.stage)}`} />
      <div className="status-text">
        <strong>{STAGES[horizon.stage] || horizon.stage}</strong>
        <span>{task ? `Running: ${task.label}` : horizon.detail || state.avd}</span>
      </div>
    </button>}
  </aside>;
}
