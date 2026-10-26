import { LogoMark } from './logo';

export const STAGES = {
  checking: 'Checking', stopped: 'Stopped', booting: 'Booting Android', clock: 'Fixing the clock',
  starting: 'Starting Horizon', ready: 'Ready',
};
export const stageTone = stage => stage === 'ready' ? 'ok' : stage === 'stopped' || stage === 'checking' ? 'off' : 'busy';

export function bytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 ** 2) return `${Math.round(n / 1024)} KB`;
  return n < 1024 ** 3 ? `${(n / 1024 ** 2).toFixed(1)} MB` : `${(n / 1024 ** 3).toFixed(2)} GB`;
}
export function ago(seconds) {
  const minutes = Math.round((Date.now() / 1000 - seconds) / 60);
  if (minutes < 2) return 'just now';
  if (minutes < 60) return `${minutes} min ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 24) return `${hours} h ago`;
  const days = Math.round(hours / 24);
  return days === 1 ? 'yesterday' : `${days} days ago`;
}

export function IconButton({ label, children, className = '', ...props }) {
  return <button type="button" className={`icon-btn ${className}`} aria-label={label} title={label} {...props}>{children}</button>;
}

export function Empty({ title, children, action }) {
  return <div className="empty">
    <LogoMark className="mark" />
    <h2>{title}</h2>
    {children && <p>{children}</p>}
    {action}
  </div>;
}

export function PageHead({ title, sub, children }) {
  return <div className="page-head">
    <div><h1 className="page-title">{title}</h1>{sub && <div className="page-sub">{sub}</div>}</div>
    {children && <div className="head-actions">{children}</div>}
  </div>;
}
