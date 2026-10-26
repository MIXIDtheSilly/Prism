import { useId } from 'react';

// The mark of img/prism_banner.png (launcher/icon.svg without its tile): the triangle in
// currentColor, the three beams in their own colours, fading into the middle.
const BEAMS = [['#a4e830', 0], ['#5592de', 120], ['#ff6570', -120]];

export function LogoMark({ className }) {
  const id = useId().replace(/[^\w-]/g, '');
  return <svg viewBox="-200 -215 400 420" className={className} aria-hidden="true">
    <defs>
      <linearGradient id={`${id}-fade`} gradientUnits="userSpaceOnUse" x1="0" y1="22" x2="0" y2="82">
        <stop offset="0" stopColor="#fff" stopOpacity="0" /><stop offset="1" stopColor="#fff" />
      </linearGradient>
      <mask id={`${id}-beam`} maskUnits="userSpaceOnUse" x="-256" y="-256" width="512" height="512">
        <rect x="-36" y="0" width="72" height="196" fill={`url(#${id}-fade)`} />
      </mask>
    </defs>
    <path d="M0 -190 L164.5 95 L-164.5 95 Z" fill="none" stroke="currentColor" strokeWidth="30" />
    {BEAMS.map(([color, angle]) => <rect key={angle} x="-36" y="0" width="72" height="196" fill={color}
      mask={`url(#${id}-beam)`} transform={`rotate(${angle})`} />)}
  </svg>;
}
