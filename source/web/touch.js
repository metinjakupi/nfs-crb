// Mobile controls use Carbon's standard keyboard bindings through SDL's existing input path.
(() => {
  'use strict';
  if (document.getElementById('carbon-touch') || !navigator.maxTouchPoints) return;
  const canvas = document.getElementById('canvas');
  if (!canvas) return;
  const style = document.createElement('style');
  style.textContent = `
    #carbon-touch { position:fixed; inset:0; z-index:20; pointer-events:none; user-select:none; -webkit-user-select:none; font:600 12px system-ui,sans-serif; }
    #carbon-touch [hidden] { display:none!important; }
    #carbon-touch .ct-group { position:absolute; display:flex; gap:8px; align-items:end; }
    #carbon-touch button { touch-action:none; pointer-events:auto; min-width:52px; min-height:52px; padding:10px; border:1px solid #b8e8ec70; border-radius:14px; background:#07151ec9; color:#e7faff; font:inherit; box-shadow:0 3px 12px #0005; -webkit-tap-highlight-color:transparent; }
    #carbon-touch button:focus-visible { outline:2px solid #83edf0; outline-offset:3px; }
    #carbon-touch button[data-held] { background:#53cbd9d9; color:#03141b; border-color:#b3ffff; }
    #carbon-touch .ct-top { top:max(12px,env(safe-area-inset-top)); right:max(12px,env(safe-area-inset-right)); }
    #carbon-touch .ct-steer { bottom:max(18px,env(safe-area-inset-bottom)); left:max(14px,env(safe-area-inset-left)); }
    #carbon-touch .ct-steer button { width:clamp(56px,10vw,84px); height:68px; font-size:24px; }
    #carbon-touch .ct-pedals { bottom:max(18px,env(safe-area-inset-bottom)); right:max(14px,env(safe-area-inset-right)); }
    #carbon-touch .ct-pedals button { height:76px; }
    #carbon-touch .ct-gas { border-color:#67dfe5; background:#164c60cf; }
    #carbon-touch .ct-actions { bottom:calc(max(18px,env(safe-area-inset-bottom)) + 86px); right:max(14px,env(safe-area-inset-right)); }
    #carbon-touch .ct-nav { left:max(14px,env(safe-area-inset-left)); bottom:calc(max(18px,env(safe-area-inset-bottom)) + 78px); }
    @media (max-height:420px) { #carbon-touch button { min-height:44px; min-width:48px; } #carbon-touch .ct-top button { min-height:44px; } }
  `;
  document.head.append(style);
  const root = document.createElement('div');
  root.id = 'carbon-touch'; root.setAttribute('aria-label', 'Touch controls');
  const held = new Map();
  const keys = new Map();
  const event = (type, binding) => {
    const [key, code, keyCode] = binding;
    canvas.dispatchEvent(new KeyboardEvent(type, {key, code, keyCode, which:keyCode, bubbles:true, cancelable:true, shiftKey:code === 'ShiftRight'}));
  };
  function release(id) {
    const item = held.get(id); if (!item) return;
    held.delete(id);
    const count = keys.get(item.binding[1]) - 1;
    if (count) keys.set(item.binding[1], count);
    else { keys.delete(item.binding[1]); event('keyup', item.binding); }
    if (![...held.values()].some(v => v.button === item.button)) delete item.button.dataset.held;
  }
  function releaseAll() { for (const id of [...held.keys()]) release(id); }
  function group(name, entries) {
    const box = document.createElement('div'); box.className = `ct-group ct-${name}`;
    for (const [label, title, binding, className] of entries) {
      const button = document.createElement('button');
      button.type = 'button'; button.textContent = label; button.setAttribute('aria-label', title);
      if (className) button.className = className;
      button.addEventListener('pointerdown', e => {
        if (e.pointerType === 'mouse' && e.button !== 0) return;
        e.preventDefault(); release(e.pointerId);
        canvas.focus({preventScroll:true});
        button.setPointerCapture(e.pointerId);
        held.set(e.pointerId, {button, binding}); button.dataset.held = '';
        const count = keys.get(binding[1]) || 0; keys.set(binding[1], count + 1);
        if (!count) event('keydown', binding);
      });
      for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) button.addEventListener(type, e => release(e.pointerId));
      button.addEventListener('contextmenu', e => e.preventDefault());
      box.append(button);
    }
    root.append(box); return box;
  }
  const top = group('top', [['Menu','Open Carbon menu',['F9','F9',120]], ['Pause','Pause or back',['Escape','Escape',27]]]);
  const driving = [
    group('steer', [['◀','Steer left',['ArrowLeft','ArrowLeft',37]], ['▶','Steer right',['ArrowRight','ArrowRight',39]]]),
    group('pedals', [['Brake','Brake or reverse',['ArrowDown','ArrowDown',40]], ['Gas','Accelerate',['ArrowUp','ArrowUp',38],'ct-gas']]),
    group('actions', [['NOS','Nitrous',['Shift','ShiftRight',16]], ['Slide','Handbrake',[' ','Space',32]]]),
    group('nav', [['↑','Menu up',['ArrowUp','ArrowUp',38]], ['↓','Menu down',['ArrowDown','ArrowDown',40]], ['OK','Confirm',['Enter','Enter',13]]]),
  ];
  document.body.append(root);
  // HTML menus own touch input. Native game menus use the arrow/OK controls.
  function sync() {
    const editing = !!document.activeElement?.closest('input,textarea,select,[contenteditable="true"]');
    const blocked = editing || !!globalThis.recompMenuOpen || document.hidden;
    for (const box of driving) if (box.hidden !== blocked) box.hidden = blocked;
    const hideTop = editing || document.hidden;
    if (top.hidden !== hideTop) top.hidden = hideTop;
    if (blocked) releaseAll();
  }
  const observer = new MutationObserver(sync);
  observer.observe(document.body, {subtree:true, attributes:true, attributeFilter:['hidden','open','class']});
  addEventListener('focusin', sync); addEventListener('focusout', sync);
  addEventListener('blur', releaseAll); addEventListener('pagehide', releaseAll);
  addEventListener('visibilitychange', sync); addEventListener('resize', releaseAll);
  addEventListener('keydown', e => { if (['F9','F10','Escape'].includes(e.key)) queueMicrotask(sync); });
  sync();
})();
