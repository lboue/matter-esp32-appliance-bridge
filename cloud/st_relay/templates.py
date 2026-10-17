"""The few HTML pages a person sees: the end of the OAuth flow."""

from __future__ import annotations

from html import escape

_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  :root {{ color-scheme: light dark; --fg:#212529; --bg:#fff; --muted:#6c757d; --accent:{accent}; }}
  @media (prefers-color-scheme: dark) {{ :root {{ --fg:#e9ecef; --bg:#121416; --muted:#9aa3ab; }} }}
  body {{ font-family: system-ui, sans-serif; background: var(--bg); color: var(--fg);
         max-width: 32rem; margin: 15vh auto; padding: 0 1rem; line-height: 1.5; }}
  h1 {{ font-size: 1.4rem; color: var(--accent); }}
  p {{ color: var(--muted); }}
</style>
</head>
<body>
<h1>{title}</h1>
<p>{message}</p>
</body>
</html>
"""


def page(title: str, message: str, ok: bool = True) -> str:
    return _PAGE.format(
        title=escape(title),
        message=escape(message),
        accent="#2e9e6b" if ok else "#c92a2a",
    )
