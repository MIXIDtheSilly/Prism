import { useEffect, useState } from 'react';
import { CircleAlert, CircleCheck, CircleHelp, FolderOpen, Loader2, Play, RefreshCw, RotateCcw, Stethoscope } from 'lucide-react';
import { call } from '../api';
import { PageHead } from '../components/common';

const STEP_ICON = { true: CircleCheck, false: CircleAlert, null: CircleHelp };

function Steps({ state, run, pending, notify }) {
  const { steps, task } = state;
  const busy = Boolean(task?.running);
  const setup = force => run(`setup-${force || ''}`, async () => {
    await call('run', 'setup', force ? { force } : {});
    notify(force ? `Running ${force} again` : 'Setting up');
  });
  const [all, setAll] = useState(false);
  const todo = steps?.filter(s => s.current === false).length || 0;
  const attention = (steps || []).filter(s => s.current !== true);
  const listed = all ? steps || [] : attention;
  return <section className="panel">
    <div className="panel-row">
      <div>
        <h2>Setup's steps</h2>
        <p className="panel-sub">Each runs only when it isn't current. {steps ? (todo ? `${todo} to do.` : 'All current.') : 'Checking…'}</p>
      </div>
      <div className="head-actions">
        <button type="button" className="icon-btn" aria-label="Check again" title="Check again" disabled={state.stepsChecking || busy}
          onClick={() => run('checkSteps', () => call('checkSteps'))}>{state.stepsChecking ? <Loader2 className="spin" /> : <RefreshCw />}</button>
        <button type="button" className="btn btn-primary" disabled={busy} onClick={() => setup()}><Play />Set up</button>
      </div>
    </div>
    <ul className="setup-list">
      {steps && !listed.length && <li className="ok all-current"><CircleCheck /><div className="setup-text"><span>All {steps.length} steps are current.</span></div></li>}
      {listed.map(step => {
        const Icon = STEP_ICON[step.current];
        const tone = step.current === true ? 'ok' : step.current === false ? 'todo' : 'unknown';
        return <li key={step.name} className={tone}>
          <Icon />
          <div className="setup-text"><strong>{step.name}</strong><span>{step.detail}</span></div>
          <button type="button" className="icon-btn icon-btn-sm redo" disabled={busy} title={`Run ${step.name} again, even if it's current`}
            aria-label={`Run ${step.name} again`} onClick={() => setup(step.name)}><RotateCcw /></button>
        </li>;
      })}
    </ul>
    {steps && <button type="button" className="btn btn-ghost btn-sm more" onClick={() => setAll(v => !v)}>
      {all ? (attention.length ? `Only the ${attention.length} needing a look` : 'Hide the steps') : `All ${steps.length} steps`}</button>}
  </section>;
}

function Doctor({ checks, setChecks, run, pending }) {
  const check = () => run('doctor', async () => setChecks(await call('doctor')));
  useEffect(() => { if (!checks) check(); }, []);  // eslint-disable-line react-hooks/exhaustive-deps
  const problems = (checks || []).filter(c => c.ok !== true);
  const fine = (checks || []).filter(c => c.ok === true);
  return <section className={`panel ${problems.length ? 'has-problems' : ''}`}>
    <div className="panel-row">
      <div>
        <h2>Check this PC</h2>
        <p className="panel-sub">{pending.has('doctor') ? 'Looking…' : !checks ? '' : problems.some(c => c.ok === false)
          ? `${problems.filter(c => c.ok === false).length} thing(s) Prism needs are missing.` : problems.length ? 'Everything Prism needs is here, with warnings.' : 'Everything Prism needs is here.'}</p>
      </div>
      <button type="button" className="btn btn-outline" disabled={pending.has('doctor')} onClick={check}>
        {pending.has('doctor') ? <Loader2 className="spin" /> : <Stethoscope />}Check again</button>
    </div>
    {problems.length > 0 && <ul className="problem-list">
      {problems.map(c => <li key={c.what} className={c.ok === false ? 'fail' : 'warn'}>
        <CircleAlert />
        <div><strong>{c.what}</strong><span>{c.detail}</span><span className="fix">{c.fix}</span></div>
      </li>)}
    </ul>}
    {fine.length > 0 && <div className="check-chips">
      {fine.map(c => <span key={c.what} title={c.detail}><CircleCheck />{c.what}</span>)}
    </div>}
  </section>;
}

function Settings({ state, run, notify }) {
  const { settings } = state;
  const [ipd, setIpd] = useState(String(settings.ipd));
  useEffect(() => setIpd(String(settings.ipd)), [settings.ipd]);
  const save = patch => run('settings', async () => { await call('settings', patch); });
  const saveIpd = () => {
    const value = Number(ipd);
    if (Number.isFinite(value) && value >= 50 && value <= 80) { if (value !== settings.ipd) save({ ipd: value }); }
    else { setIpd(String(settings.ipd)); notify('The IPD is 50 to 80 mm', true); }
  };
  return <section className="panel">
    <h2>Settings</h2>
    <p className="panel-sub">Kept in work\prism.json, which prism.py uses too.</p>
    <div className="field">
      <label htmlFor="ota">Horizon OS OTA</label>
      <div className="field-row">
        <input id="ota" className="field-input mono" readOnly value={settings.ota || ''} placeholder={state.prepared ? 'The workspace is prepared; no OTA needed' : "Your headset's full OTA (.zip)"} />
        <button type="button" className="btn btn-outline" onClick={() => run('pickOta', () => call('pickOta'))}><FolderOpen />Browse</button>
      </div>
    </div>
    <div className="fields-2">
      <div className="field">
        <label>Display refresh rate</label>
        <div className="segmented" role="group">
          {[60, 90].map(rate => <button key={rate} type="button" aria-pressed={settings.refresh === rate}
            onClick={() => save({ refresh: rate })}>{rate} Hz</button>)}
        </div>
      </div>
      <div className="field">
        <label htmlFor="ipd">IPD (mm)</label>
        <input id="ipd" className="field-input" inputMode="decimal" value={ipd} onChange={e => setIpd(e.target.value)}
          onBlur={saveIpd} onKeyDown={e => e.key === 'Enter' && e.currentTarget.blur()} />
      </div>
    </div>
    <label className="toggle-row">
      <div><strong>Left menu button is Horizon's menu button</strong><span>Otherwise it's the Meta button.</span></div>
      <input type="checkbox" className="switch" checked={settings.menu_button_is_menu}
        onChange={e => save({ menu_button_is_menu: e.target.checked })} />
    </label>
  </section>;
}

function SteamVR({ state, run, notify }) {
  const busy = Boolean(state.task?.running);
  return <section className="panel">
    <div className="panel-row">
      <div>
        <h2>SteamVR's library</h2>
        <p className="panel-sub">{state.registered ? 'Prism is listed as Horizon OS (Prism), so it starts from the headset too.'
          : 'List Prism in SteamVR as Horizon OS (Prism), to start it from the headset.'}</p>
      </div>
      <button type="button" className="btn btn-outline" disabled={busy || state.registered == null}
        onClick={() => run('library', async () => { await call('run', 'library'); notify(state.registered ? 'Removing Prism from SteamVR' : 'Adding Prism to SteamVR'); })}>
        {state.registered ? 'Remove' : 'Add'}</button>
    </div>
  </section>;
}

export function Setup(props) {
  return <div className="page">
    <PageHead title="Setup" sub="What Prism needs, how far setup got, and its settings." />
    <div className="columns">
      <div className="column">
        <Steps {...props} />
      </div>
      <div className="column">
        <Doctor {...props} />
        <Settings {...props} />
        <SteamVR {...props} />
      </div>
    </div>
  </div>;
}
