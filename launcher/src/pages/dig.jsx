import { useEffect, useMemo, useRef, useState } from 'react';
import { ArrowDownToLine, CornerDownLeft, Eraser, FileArchive, FileText, FolderOpen, Loader2, Pause, Play, RefreshCw } from 'lucide-react';
import { call, onLogcat } from '../api';
import { ago, bytes, Empty, PageHead } from '../components/common';
import { Chip, Highlight, LineView, LogList, SearchBox, useMatcher } from '../components/log';

// --- logcat ---------------------------------------------------------------------------------------
// Rows ([time, pid, tid, level, tag, message]) are kept here, outside the page, so they survive a
// visit to another page; the backend sends them only while the Logcat tab is open.

const MAX_ROWS = 50000;
const LEVELS = ['V', 'D', 'I', 'W', 'E', 'F'];
const logcat = { rows: [], version: 0, listeners: new Set(), subscribed: false };

function useLogcatRows() {
  const [version, setVersion] = useState(logcat.version);
  useEffect(() => {
    if (!logcat.subscribed) {
      logcat.subscribed = true;
      onLogcat(rows => {
        for (const row of rows) logcat.rows.push(row);
        if (logcat.rows.length > MAX_ROWS) logcat.rows.splice(0, logcat.rows.length - MAX_ROWS);
        logcat.version++;
        logcat.listeners.forEach(listener => listener(logcat.version));
      });
    }
    logcat.listeners.add(setVersion);
    return () => logcat.listeners.delete(setVersion);
  }, []);
  return version;
}

function Logcat({ state }) {
  const version = useLogcatRows();
  const [following, setFollowing] = useState(true);
  const [follow, setFollow] = useState(true);
  const [level, setLevel] = useState('V');
  const [tag, setTag] = useState('');
  const [pid, setPid] = useState('');
  const [selected, setSelected] = useState(null);
  const matcher = useMatcher();
  useEffect(() => { call('logcat', following).catch(() => {}); }, [following]);
  useEffect(() => () => { call('logcat', false).catch(() => {}); }, []);

  const min = LEVELS.indexOf(level);
  const shown = useMemo(() => {
    const out = [];
    const rows = logcat.rows;
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i];
      if (r[3] !== '-' && LEVELS.indexOf(r[3]) < min) continue;
      if (tag && r[4] !== tag) continue;
      if (pid && r[1] !== pid) continue;
      if (matcher.active && !matcher.test(r[4]) && !matcher.test(r[5])) continue;
      out.push(i);
    }
    return out;
  }, [version, min, tag, pid, matcher.pattern]);  // eslint-disable-line react-hooks/exhaustive-deps
  const row = selected != null ? logcat.rows[selected] : null;

  return <div className="log-pane">
    <div className="log-toolbar">
      <SearchBox matcher={matcher} placeholder="Search tags and messages" />
      <select className="field-input compact" value={level} onChange={e => setLevel(e.target.value)} aria-label="Lowest level">
        {LEVELS.map(l => <option key={l} value={l}>{{ V: 'Verbose', D: 'Debug', I: 'Info', W: 'Warning', E: 'Error', F: 'Fatal' }[l]} and up</option>)}
      </select>
      <div className="grow" />
      <button type="button" className="icon-btn" aria-pressed={following} title={following ? 'Pause' : 'Follow logcat again'}
        aria-label={following ? 'Pause' : 'Resume'} onClick={() => setFollowing(v => !v)}>{following ? <Pause /> : <Play />}</button>
      <button type="button" className="icon-btn" title="Scroll to the newest" aria-label="Scroll to the newest" onClick={() => setFollow(true)}><ArrowDownToLine /></button>
      <button type="button" className="icon-btn" title="Clear" aria-label="Clear" onClick={() => { logcat.rows = []; logcat.version++; setSelected(null); logcat.listeners.forEach(l => l(logcat.version)); }}><Eraser /></button>
    </div>
    <div className="log-status">
      <span className="muted">{shown.length.toLocaleString()} of {logcat.rows.length.toLocaleString()} lines{following ? '' : ' · paused'}
        {state.horizon.stage === 'stopped' ? " · the emulator isn't running" : ''}</span>
      {tag && <Chip onRemove={() => setTag('')}>tag {tag}</Chip>}
      {pid && <Chip onRemove={() => setPid('')}>pid {pid}</Chip>}
      {!tag && !pid && <span className="faint">Click a tag or a pid to show only its lines.</span>}
    </div>
    <LogList count={shown.length} follow={follow} setFollow={setFollow} label="Logcat" empty={following ? 'Waiting for logcat' : 'Paused'}
      renderRow={(i, style) => {
        const index = shown[i], r = logcat.rows[index];
        return <div key={index} style={style} className={`log-row lv-${r[3]} ${selected === index ? 'selected' : ''}`} onClick={() => setSelected(index)}>
          <span className="c-time">{r[0]}</span>
          <span className="c-pid link" onClick={e => { e.stopPropagation(); setPid(r[1]); }}>{r[1]}</span>
          <span className="c-lv">{r[3] === '-' ? '' : r[3]}</span>
          <span className="c-tag link" title={r[4]} onClick={e => { e.stopPropagation(); setTag(r[4]); }}>{r[4]}</span>
          <span className="c-msg"><Highlight text={r[5]} pattern={matcher.pattern} /></span>
        </div>;
      }} />
    {row && <div className="log-detail">
      <div className="log-detail-head"><span className="mono">{row[0]} · pid {row[1]} tid {row[2]} · {row[3]} · {row[4]}</span><div className="grow" />
        <button type="button" className="btn btn-ghost btn-sm" onClick={() => navigator.clipboard.writeText(row.join(' '))}>Copy</button>
        <button type="button" className="btn btn-ghost btn-sm" onClick={() => setSelected(null)}>Close</button></div>
      <pre className="selectable">{row[5]}</pre>
    </div>}
  </div>;
}

// --- shell ----------------------------------------------------------------------------------------

const PRESETS = [
  ['Crashes', 'logcat -d -b crash -t 300'],
  ['Tombstones', 'ls -lt /data/tombstones /data/anr'],
  ['Top', 'top -b -n 1 -m 25'],
  ['Memory', 'head -n 8 /proc/meminfo; echo; dumpsys meminfo -s | head -n 40'],
  ['Focus', "dumpsys activity activities | grep -E 'topResumedActivity|ResumedActivity' | head -n 5; dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'"],
  ['Boot', 'getprop sys.boot_completed; cat /sys/devices/system/clocksource/clocksource0/current_clocksource; uptime'],
  ['Meta props', "getprop | grep -iE 'oculus|horizon|meta|vr' | head -n 120"],
];
const shellStore = { blocks: [], history: [] };

function Shell({ state, run, pending }) {
  const [blocks, setBlocks] = useState(shellStore.blocks);
  const [command, setCommand] = useState('');
  const [back, setBack] = useState(-1);
  const out = useRef(null);
  const busy = pending.has('shell');
  useEffect(() => { out.current?.scrollTo({ top: out.current.scrollHeight }); }, [blocks]);
  const exec = text => {
    text = text.trim();
    if (!text) return;
    shellStore.history = [text, ...shellStore.history.filter(h => h !== text)].slice(0, 100);
    setBack(-1); setCommand('');
    run('shell', async () => {
      let block;
      try { block = await call('shell', text); }
      catch (e) { block = { command: text, code: null, output: String(e?.message || e), ms: 0 }; }
      shellStore.blocks = [...shellStore.blocks, block].slice(-60);
      setBlocks(shellStore.blocks);
    });
  };
  const onKey = e => {
    if (e.key === 'Enter') { if (!busy) exec(command); }
    else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      const next = Math.max(-1, Math.min(shellStore.history.length - 1, back + (e.key === 'ArrowUp' ? 1 : -1)));
      setBack(next); setCommand(next < 0 ? '' : shellStore.history[next]);
    }
  };
  return <div className="log-pane">
    <div className="quick">
      {PRESETS.map(([label, text]) => <button key={label} type="button" className="tag-btn" title={text} disabled={busy} onClick={() => exec(text)}>{label}</button>)}
      <div className="grow" />
      {blocks.length > 0 && <button type="button" className="btn btn-ghost btn-sm" onClick={() => { shellStore.blocks = []; setBlocks([]); }}><Eraser />Clear</button>}
    </div>
    <div className="shell-out selectable" ref={out}>
      {!blocks.length && <div className="log-empty">adb shell on {state.serial}. Commands time out after 60 s.</div>}
      {blocks.map((b, i) => <div key={i} className="shell-block">
        <div className="shell-cmd"><span>#</span>{b.command}<small className={b.code ? 'bad' : 'muted'}>{b.code == null ? 'failed' : `exit ${b.code} · ${b.ms} ms`}</small></div>
        <pre>{b.output || '(no output)'}</pre>
      </div>)}
    </div>
    <div className="shell-input">
      <span>#</span>
      <input className="field-input mono" placeholder="A shell command for Horizon (↑ for earlier ones)" value={command} spellCheck={false}
        onChange={e => setCommand(e.target.value)} onKeyDown={onKey} />
      <button type="button" className="icon-btn" aria-label="Run" disabled={busy || !command.trim()} onClick={() => exec(command)}>
        {busy ? <Loader2 className="spin" /> : <CornerDownLeft />}</button>
    </div>
  </div>;
}

// --- Prism's own logs -----------------------------------------------------------------------------

function Files({ state, run, pending, notify }) {
  const [files, setFiles] = useState(null);
  const [open, setOpen] = useState(null);
  const matcher = useMatcher();
  const list = () => run('logs', async () => setFiles(await call('logs')));
  const read = name => run('readLog', async () => { const log = await call('readLog', name); setOpen({ ...log, lines: log.text.split('\n') }); });
  useEffect(() => { list(); }, []);  // eslint-disable-line react-hooks/exhaustive-deps
  const reporting = state.task?.name === 'report' && state.task.running;
  return <div className="files">
    <div className="file-side">
      <div className="tools">
        <button type="button" className="btn btn-outline btn-sm" disabled={reporting || state.task?.running}
          onClick={() => run('report', async () => { await call('run', 'report'); notify('Making a report'); })}
          title="A zip of the checks and logs to attach to an issue, user and computer names replaced">
          {reporting ? <Loader2 className="spin" /> : <FileArchive />}Make a report</button>
        {state.lastReport && <button type="button" className="btn btn-ghost btn-sm" onClick={() => run('open', () => call('open', 'report'))}>Show it</button>}
        <div className="grow" />
        <button type="button" className="icon-btn icon-btn-sm" aria-label="Open the logs folder" title="Open work\logs" onClick={() => run('open', () => call('open', 'logs'))}><FolderOpen /></button>
        <button type="button" className="icon-btn icon-btn-sm" aria-label="Refresh" title="Refresh" onClick={list}>{pending.has('logs') ? <Loader2 className="spin" /> : <RefreshCw />}</button>
      </div>
      <ul className="file-list">
        {(files || []).map(f => <li key={f.name}>
          <button type="button" aria-current={open?.name === f.name ? 'true' : undefined} onClick={() => read(f.name)}>
            <FileText /><span className="name">{f.name}</span><small>{bytes(f.size)} · {ago(f.modified)}</small>
          </button>
        </li>)}
        {files && !files.length && <li className="muted">No logs yet</li>}
      </ul>
    </div>
    <div className="log-pane">
      {open ? <>
        <div className="log-toolbar">
          <SearchBox matcher={matcher} placeholder={`Search ${open.name}`} />
          <div className="grow" />
          <span className="muted small">{bytes(open.size)}{open.cut ? ', its last 2 MB' : ''}</span>
          <button type="button" className="icon-btn icon-btn-sm" aria-label="Read again" title="Read again" onClick={() => read(open.name)}><RefreshCw /></button>
        </div>
        <LineView key={open.name} lines={open.lines} matcher={matcher} toned={open.name.startsWith('window-')} label={open.name} empty="Empty" />
      </> : <Empty title="Prism's logs">work\logs: the emulator's own output, each window command's, SteamVR's runs and the soak and perf records.</Empty>}
    </div>
  </div>;
}

const TABS = [['logcat', 'Logcat'], ['shell', 'Shell'], ['files', 'Logs']];

export function Dig(props) {
  const [tab, setTab] = useState('logcat');
  return <div className="page wide">
    <PageHead title="Dig" sub="Horizon's logcat, a shell on it, and Prism's own logs.">
      <div className="segmented" role="tablist">
        {TABS.map(([id, label]) => <button key={id} type="button" role="tab" aria-pressed={tab === id} aria-selected={tab === id} onClick={() => setTab(id)}>{label}</button>)}
      </div>
    </PageHead>
    {tab === 'logcat' ? <Logcat {...props} /> : tab === 'shell' ? <Shell {...props} /> : <Files {...props} />}
  </div>;
}
