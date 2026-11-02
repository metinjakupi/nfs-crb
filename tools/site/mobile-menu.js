"use strict";
// Mobile beta only. Use the same keyboard path as a physical keyboard.
(() => {
  const style = document.createElement('style');
  style.textContent = `
    .mobile-menu { position:fixed; z-index:90; left:0; right:0; bottom:0;
      padding:6px max(8px,env(safe-area-inset-right)) max(6px,env(safe-area-inset-bottom)) max(8px,env(safe-area-inset-left));
      display:flex; justify-content:center; gap:6px; background:rgba(7,12,22,.94);
      border-top:1px solid #36515f; font:600 14px system-ui; }
    .mobile-menu[hidden] { display:none; }
    .mobile-menu button { min-width:48px; min-height:48px; padding:8px 12px;
      border:1px solid #53717c; border-radius:8px; background:#182b37; color:#e9f6ff;
      font:inherit; touch-action:manipulation; user-select:none; -webkit-user-select:none; }
    .mobile-menu button:last-child { background:#5fe4f2; color:#070c16; border-color:#5fe4f2; }
    .mobile-menu button:active { background:#487080; }
    .mobile-menu button:focus-visible { outline:3px solid white; outline-offset:2px; }
    .mobile-menu-game { position:fixed!important; top:0!important; left:0!important;
      width:100%!important; height:calc(100dvh - var(--mobile-menu-height,66px))!important;
      object-fit:contain; }
    @media(max-width:420px) { .mobile-menu { gap:4px; } .mobile-menu button { padding:8px; } }
  `;
  document.head.append(style);
  const bar = document.createElement('nav');
  bar.className = 'mobile-menu'; bar.hidden = true;
  bar.setAttribute('aria-label', 'Game menu controls');
  const held = new Map();
  const send = (type, key, code, keyCode) => {
    const canvas = document.getElementById('canvas');
    canvas?.focus({preventScroll:true});
    (canvas || document).dispatchEvent(new KeyboardEvent(type, {
      key, code, keyCode, which:keyCode, bubbles:true, cancelable:true,
    }));
  };
  const release = () => {
    for (const [code, entry] of held) {
      clearTimeout(entry.timer); send('keyup', entry.key, code, entry.keyCode);
    }
    held.clear();
  };
  for (const [label, key, code, keyCode, name] of [
    ['Back','Escape','Escape',27,'Back'],
    ['←','ArrowLeft','ArrowLeft',37,'Left'],
    ['↑','ArrowUp','ArrowUp',38,'Up'],
    ['↓','ArrowDown','ArrowDown',40,'Down'],
    ['→','ArrowRight','ArrowRight',39,'Right'],
    ['Select','Enter','Enter',13,'Select or continue'],
  ]) {
    const button = document.createElement('button');
    button.type = 'button'; button.textContent = label;
    button.setAttribute('aria-label',name); button.dataset.code = code;
    button.addEventListener('click', () => {
      if (held.has(code)) return;
      send('keydown',key,code,keyCode);
      const timer = setTimeout(() => {
        held.delete(code); send('keyup',key,code,keyCode);
      },120);
      held.set(code,{key,keyCode,timer});
    });
    bar.append(button);
  }
  document.body.append(bar);
  const layout = () => document.documentElement.style.setProperty('--mobile-menu-height', `${bar.getBoundingClientRect().height}px`);
  new ResizeObserver(layout).observe(bar);
  addEventListener('blur',release);
  document.addEventListener('visibilitychange',() => {if(document.hidden) release();});
  addEventListener('recomp-start-page',() => {
    release(); bar.hidden = true; document.getElementById('canvas')?.classList.remove('mobile-menu-game');
  });
  window.carbonMobileMenuReady = () => {
    bar.hidden = false;
    document.getElementById('canvas')?.classList.add('mobile-menu-game'); layout();
  };
})();
