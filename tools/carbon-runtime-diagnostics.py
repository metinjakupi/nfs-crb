from pathlib import Path
root=Path(r'C:\Users\Metin\projects\carbon-browser\source')
p=root/'kit/tools/web_launcher.py';s=p.read_text()
s=s.replace('import sys\n','import sys\nimport threading\n',1)
needle='    def end_headers(self):\n'
assert needle in s and 'def do_POST(self)' not in s
s=s.replace(needle,'''    _diagnostic_lock = threading.Lock()

    def do_POST(self):
        # Private localhost development log: only our player sends runtime errors.
        if self.path != "/_runtime-diagnostics":
            self.send_error(404)
            return
        origin = self.headers.get("Origin", "")
        if origin and origin != "http://" + self.headers.get("Host", ""):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 16384:
                self.send_error(413)
                return
            record = json.loads(self.rfile.read(length))
            if not isinstance(record, dict):
                raise ValueError("expected object")
        except (ValueError, UnicodeError):
            self.send_error(400)
            return
        path = Path(self.directory).parent / "browser-runtime-diagnostics.jsonl"
        with self._diagnostic_lock:
            with path.open("a", encoding="utf-8") as log:
                log.write(json.dumps(record, ensure_ascii=True) + "\\n")
        self.send_response(204)
        self.end_headers()

'''+needle,1)
p.write_text(s)
p=root/'kit/web/player/index.html';s=p.read_text()
needle='window.recompLog = log; // for tests\n'
assert needle in s and 'function reportFailure' not in s
s=s.replace(needle,needle+'''
let reportingFailure = false;
function reportFailure(kind, error) {
  const message = String(error?.message || error || "Unknown startup error");
  status(`The game stopped: ${message}`, true);
  if (reportingFailure) return;
  reportingFailure = true;
  const record = {
    kind, message: message.slice(0, 1000),
    stack: String(error?.stack || "").slice(0, 5000),
    game: location.pathname, time: new Date().toISOString(),
    log: log.slice(-20).map(t => String(t).slice(0, 300)),
  };
  fetch("/_runtime-diagnostics", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify(record), keepalive: true,
  }).catch(() => {});
}
window.addEventListener("error", e => reportFailure("error", e.error || e.message));
window.addEventListener("unhandledrejection", e => reportFailure("rejection", e.reason));
''',1)
s=s.replace('onAbort: (what) => status(`The game stopped: ${what}`, true),','onAbort: (what) => reportFailure("abort", what),',1)
s=s.replace('Module.callMain([`--exe=${exe}`]);','try { Module.callMain([`--exe=${exe}`]); }\n      catch (error) { reportFailure("main", error); }',1)
p.write_text(s)
print('Local runtime error reporting added')
