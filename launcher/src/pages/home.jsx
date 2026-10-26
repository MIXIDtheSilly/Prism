import { CircleAlert, Crosshair, Glasses, Loader2, Minus, Monitor, Plus, Power, X } from 'lucide-react';
import { call } from '../api';
import { PageHead, STAGES, stageTone } from '../components/common';
import { LineView, SearchBox, useMatcher } from '../components/log';

function Tile({ label, value, detail, tone }) {
  return <div className={`tile ${tone || ''}`}>
    <small>{label}</small>
    <strong>{value}</strong>
    <span title={detail}>{detail || ' '}</span>
  </div>;
}

function bridgeTile(bridge) {
  if (!bridge) return { value: 'Not running', detail: 'Start VR or Desktop' };
  const mode = bridge.mode === 'vr' ? 'VR' : 'Desktop';
  if (bridge.horizon == null) return { value: mode, detail: bridge.state, tone: 'busy' };
  return {
    value: `${bridge.horizon.toFixed(0)} fps`, tone: bridge.state === 'focused' && bridge.tracking ? 'good' : 'busy',
    detail: `${mode}: ${bridge.state}, ${bridge.tracking ? 'tracking' : 'no tracking'}, ${bridge.submitted.toFixed(0)} to the headset`,
  };
}

/** While VR runs: the controls the bridge takes (prism_vr --control), ours or SteamVR's. */
function Headset({ state, run }) {
  const latency = state.settings.latency;
  const send = line => run(`bridge-${line}`, () => call('bridge', line));
  const hold = name => ({
    onPointerDown: e => { e.currentTarget.setPointerCapture(e.pointerId); send(`${name} down`); },
    onPointerUp: () => send(`${name} up`),
    onPointerCancel: () => send(`${name} up`),
  });
  return <section className="panel">
    <div className="panel-row">
      <div><h2>Headset</h2><p className="panel-sub">Goes to the VR bridge{state.bridge?.external ? ' SteamVR started' : ''}.</p></div>
      <div className="headset-controls">
        <div className="stepper" role="group" aria-label="Latency">
          <button type="button" aria-label="Less latency" onClick={() => send(`latency ${Math.max(0, latency - 5)}`)}><Minus /></button>
          <span title="From the pose a frame is rendered with to the PC">{latency.toFixed(0)} ms</span>
          <button type="button" aria-label="More latency" onClick={() => send(`latency ${latency + 5}`)}><Plus /></button>
        </div>
        <button type="button" className="btn btn-outline btn-sm" onClick={() => send('recenter')}><Crosshair />Recenter</button>
        <button type="button" className="btn btn-outline btn-sm" title="Hold to press" {...hold('meta')}>Meta button</button>
        <button type="button" className="btn btn-outline btn-sm" title="Hold to press" {...hold('menu')}>Menu button</button>
      </div>
    </div>
  </section>;
}

export function Home({ state, run, pending, output, setPage, notify }) {
  const { horizon, bridge, task, steps } = state;
  const busy = Boolean(task?.running);
  const matcher = useMatcher();
  const todo = steps?.filter(s => s.current === false) || [];
  const unknown = steps?.filter(s => s.current === null) || [];
  const start = mode => run(mode, async () => { await call('run', mode); notify(mode === 'vr' ? 'Starting VR' : 'Starting the desktop window'); });
  const b = bridgeTile(bridge);
  const noOta = !state.prepared && !state.settings.ota;

  return <div className="page">
    <PageHead title="Horizon" sub={`${STAGES[horizon.stage] || horizon.stage}${horizon.detail ? ` · ${horizon.detail}` : ''}`}>
      <button type="button" className="btn btn-primary btn-lg" disabled={busy || bridge?.external || noOta || pending.has('vr')} onClick={() => start('vr')}>
        <Glasses />Start VR</button>
      <button type="button" className="btn btn-outline btn-lg" disabled={busy || bridge?.external || noOta || pending.has('desktop')} onClick={() => start('desktop')}>
        <Monitor />Desktop</button>
      <button type="button" className="btn btn-ghost btn-lg" title="Ends what runs and shuts the emulator down"
        disabled={(horizon.stage === 'stopped' && !busy) || task?.name === 'stop' && busy || pending.has('stop')}
        onClick={() => run('stop', async () => { await call('stop'); notify('Stopping Horizon'); })}><Power />Stop</button>
    </PageHead>

    {noOta && <div className="banner">
      <CircleAlert />
      <div><strong>Choose your OTA first</strong><span>Prism sets itself up from your own headset's full Horizon OS OTA.</span></div>
      <button type="button" className="btn btn-primary btn-sm" onClick={() => setPage('setup')}>Go to Setup</button>
    </div>}
    {!noOta && todo.length > 0 && !busy && <div className="banner">
      <CircleAlert />
      <div><strong>{todo.length} setup step{todo.length === 1 ? '' : 's'} to do</strong><span>{todo.map(s => s.name).join(', ')}: a start runs {todo.length === 1 ? 'it' : 'them'} first.</span></div>
      <button type="button" className="btn btn-outline btn-sm" onClick={() => setPage('setup')}>Look</button>
    </div>}

    <div className="tiles">
      <Tile label="Horizon" value={STAGES[horizon.stage] || horizon.stage} detail={horizon.detail || state.serial}
        tone={{ ok: 'good', busy: 'busy', off: '' }[stageTone(horizon.stage)]} />
      <Tile label="Bridge" {...b} />
      <Tile label="Setup" value={!steps ? 'Checking' : todo.length ? `${todo.length} to do` : 'Current'}
        detail={todo.length ? todo.map(s => s.name).join(', ') : unknown.length ? `${unknown.length} checked once Horizon runs` : `${steps?.length || 0} steps`}
        tone={steps && (todo.length ? 'busy' : 'good')} />
      <Tile label="SteamVR" value={state.steamvr ? 'Running' : 'Off'}
        detail={state.registered ? 'Prism is in its library' : state.registered === false ? 'Prism is not in its library' : ''} tone={state.steamvr ? 'good' : ''} />
    </div>

    {bridge?.mode === 'vr' && <Headset state={state} run={run} />}

    <section className="panel output-panel">
      <div className="panel-row">
        <div>
          <h2>Output</h2>
          <p className="panel-sub">{task ? <>{task.running && <Loader2 className="spin inline" />}{task.label}{task.running ? '' : task.code === 0 ? ': done' : `: exit ${task.code}`} · {task.log}</> : 'What setup, a start or an install prints shows here.'}</p>
        </div>
        <div className="head-actions">
          <SearchBox matcher={matcher} placeholder="Search the output" />
          {busy && <button type="button" className="btn btn-outline btn-sm" onClick={() => run('cancel', () => call('cancel'))}><X />Cancel</button>}
        </div>
      </div>
      <LineView lines={output} matcher={matcher} toned label="Output" empty="Nothing has run yet" className="short" />
    </section>
  </div>;
}
