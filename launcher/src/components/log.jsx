import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { CaseSensitive, Regex, Search, X } from 'lucide-react';

const ROW = 20;

// A search box's text as a test for lines and a pattern for highlighting. Plain text matches anywhere;
// with regex on, an invalid pattern is reported instead of matching nothing silently.
export function useMatcher() {
  const [query, setQuery] = useState('');
  const [regex, setRegex] = useState(false);
  const [matchCase, setMatchCase] = useState(false);
  let pattern = null, error = '';
  if (query) {
    try { pattern = new RegExp(regex ? query : query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), matchCase ? 'g' : 'gi'); }
    catch (e) { error = e.message.replace(/^Invalid regular expression: /, ''); }
  }
  const test = text => { if (!pattern) return true; pattern.lastIndex = 0; return pattern.test(text); };
  return { query, setQuery, regex, setRegex, matchCase, setMatchCase, pattern, error, test, active: Boolean(pattern) };
}

export function SearchBox({ matcher, placeholder = 'Search', label = 'Search' }) {
  return <div className={`log-search ${matcher.error ? 'invalid' : ''}`}>
    <Search />
    <input type="search" className="field-input" aria-label={label} placeholder={placeholder} value={matcher.query} spellCheck={false}
      onChange={e => matcher.setQuery(e.target.value)} title={matcher.error || undefined} />
    <button type="button" className="toggle-chip" aria-pressed={matcher.matchCase} title="Match case" aria-label="Match case" onClick={() => matcher.setMatchCase(v => !v)}><CaseSensitive /></button>
    <button type="button" className="toggle-chip" aria-pressed={matcher.regex} title="Regular expression" aria-label="Regular expression" onClick={() => matcher.setRegex(v => !v)}><Regex /></button>
  </div>;
}

export function Highlight({ text, pattern }) {
  if (!pattern || !text) return text;
  const parts = [];
  let last = 0;
  pattern.lastIndex = 0;
  for (let m; (m = pattern.exec(text)) && parts.length < 200;) {
    if (!m[0]) { pattern.lastIndex++; continue; }
    if (m.index > last) parts.push(text.slice(last, m.index));
    parts.push(<mark key={m.index}>{m[0]}</mark>);
    last = m.index + m[0].length;
  }
  if (!parts.length) return text;
  parts.push(text.slice(last));
  return parts;
}

export function Chip({ children, onRemove }) {
  return <span className="filter-chip">{children}<button type="button" aria-label="Remove filter" onClick={onRemove}><X /></button></span>;
}

// Only the rows in view are in the DOM, so a 50,000-line log scrolls smoothly. With `follow`
// on it sticks to the newest line; scrolling up turns it off, scrolling back to the end turns it on.
export function LogList({ count, renderRow, follow, setFollow, empty, label, className = '' }) {
  const box = useRef(null);
  const [view, setView] = useState({ top: 0, height: 600 });
  useEffect(() => {
    const element = box.current;
    const observer = new ResizeObserver(() => setView(v => ({ ...v, height: element.clientHeight })));
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useLayoutEffect(() => {
    if (follow && box.current) box.current.scrollTop = box.current.scrollHeight;
  }, [count, follow]);
  const onScroll = () => {
    const element = box.current;
    setView({ top: element.scrollTop, height: element.clientHeight });
    const atEnd = element.scrollHeight - element.scrollTop - element.clientHeight < ROW * 2;
    if (atEnd !== follow) setFollow(atEnd);
  };
  const first = Math.max(0, Math.floor(view.top / ROW) - 10);
  const last = Math.min(count, Math.ceil((view.top + view.height) / ROW) + 10);
  const rows = [];
  for (let i = first; i < last; i++) rows.push(renderRow(i, { position: 'absolute', top: i * ROW, height: ROW }));
  return <div className={`log-view ${className}`} ref={box} onScroll={onScroll} role="log" aria-label={label} tabIndex={0}>
    {count ? <div style={{ height: count * ROW, position: 'relative' }}>{rows}</div> : <div className="log-empty">{empty}</div>}
  </div>;
}

// How a line of a task's output (prism.py's, adb's) is coloured: "==" progress, "> " a command
// started, "< " one ended, and failures.
export function outputTone(line) {
  if (line.startsWith('== ')) return 'out-progress';
  if (line.startsWith('> ')) return 'out-start';
  if (line.startsWith('< ')) return /: done$/.test(line) ? 'out-done' : 'out-fail';
  if (/\b(FAIL|failed|error|Error|Traceback|Exception)\b/.test(line)) return 'out-error';
  if (/^warn\b|\bwarning\b/i.test(line)) return 'out-warn';
  return '';
}

/** Plain lines (a task's output, a log file), searchable, the matches only or all of them. */
export function LineView({ lines, label, empty, matcher, toned = false, className = '' }) {
  const [follow, setFollow] = useState(true);
  const shown = matcher?.active ? lines.filter(matcher.test) : lines;
  return <LogList count={shown.length} follow={follow} setFollow={setFollow} label={label} empty={empty} className={className}
    renderRow={(i, style) => <div key={i} style={style} title={shown[i].length > 120 ? shown[i] : undefined} className={`log-row line ${toned ? outputTone(shown[i]) : ''}`}>
      <span className="c-msg"><Highlight text={shown[i]} pattern={matcher?.pattern} /></span>
    </div>} />;
}
