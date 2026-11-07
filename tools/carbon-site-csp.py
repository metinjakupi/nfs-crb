#!/usr/bin/env python3
# carbon-site-csp.py DIR SERVER: prints the public site's Content-Security-Policy for the pages
# in DIR, the online server being SERVER. tools/carbon-site-deploy.sh runs it on the final,
# minified upload, so the hashes match what players get. Inline scripts are allowed by hash and
# no other way: a script injected into a page does not run. JSON-LD is data, not a script.
import base64, hashlib, pathlib, re, sys

SCRIPT = re.compile(r"<script\b([^>]*)>(.*?)</script>", re.S | re.I)


def inline_hashes(root):
    found = set()
    for page in pathlib.Path(root).rglob("*.html"):
        for attrs, body in SCRIPT.findall(page.read_text(encoding="utf-8")):
            if re.search(r"\bsrc\s*=", attrs, re.I) or "ld+json" in attrs.lower():
                continue
            digest = base64.b64encode(hashlib.sha256(body.encode("utf-8")).digest()).decode()
            found.add(f"'sha256-{digest}'")
    return sorted(found)


def policy(root, server):
    scripts = " ".join(["'self'", "'wasm-unsafe-eval'", "https://static.cloudflareinsights.com", *inline_hashes(root)])
    return "; ".join([
        "default-src 'self'",
        f"script-src {scripts}",
        "worker-src 'self' blob:",
        f"connect-src 'self' data: blob: https://assets.nfscrb.xyz https://{server} wss://{server} https://cloudflareinsights.com",
        "img-src 'self' data: blob: https://assets.nfscrb.xyz",
        "style-src 'self' 'unsafe-inline'",
        "font-src 'self'",
        "media-src 'self' blob:",
        "object-src 'none'",
        "base-uri 'self'",
        "form-action 'self'",
        "frame-ancestors 'none'",
    ])


if __name__ == "__main__":
    print(policy(sys.argv[1], sys.argv[2]))
