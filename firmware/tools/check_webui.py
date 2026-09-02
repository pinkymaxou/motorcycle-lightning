#!/usr/bin/env python3
"""Static sanity check for the config page (webui/).

The page has no bundler and no test runner, so a symbol deleted during a
refactor only shows up as a blank page on the bike. Two checks catch that
class of mistake before the asset is embedded:
  1. every $('id') resolves to an id present in the HTML;
  2. every called identifier is defined in the file (or is a known builtin).

Comments and string literals are blanked first so prose cannot masquerade as
code, but template substitutions (`${...}`) are kept: most of this page's
markup builders live in there.
"""
import pathlib
import re
import sys

BUILTINS = {
    'Array', 'Boolean', 'Error', 'JSON', 'Math', 'Number', 'Object', 'Promise',
    'String', 'Uint8Array', 'TextDecoder', 'TextEncoder', 'WebSocket', 'Set',
    'Map', 'Date', 'parseInt', 'parseFloat', 'isNaN', 'fetch', 'alert',
    'confirm', 'setTimeout', 'setInterval', 'clearTimeout', 'clearInterval',
    'requestAnimationFrame', 'console', 'encodeURIComponent',
    'XMLHttpRequest', 'FileReader', 'Blob', 'URL',
}
KEYWORDS = {
    'if', 'for', 'while', 'switch', 'catch', 'return', 'function', 'typeof',
    'new', 'await', 'else', 'do', 'throw', 'var', 'let', 'const', 'async',
    'delete', 'void', 'in', 'of', 'yield',
}


def strip_noise(js):
    """Blank comments and literal text, keep code inside `${...}`."""
    out = []
    stack = []             # 'tmpl' inside a template, ['sub', depth] inside ${}
    i, n = 0, len(js)
    while i < n:
        c, two = js[i], js[i:i + 2]
        top = stack[-1][0] if stack else 'code'

        if 'tmpl' == top:
            if '\\' == c:
                i += 2
            elif '${' == two:
                stack.append(['sub', 0])
                out.append(' ')
                i += 2
            elif '`' == c:
                stack.pop()
                i += 1
            else:
                if '\n' == c:
                    out.append('\n')
                i += 1
            continue

        if '//' == two:
            while i < n and '\n' != js[i]:
                i += 1
            continue
        if '/' == c and 'code' == top or '/' == c and 'sub' == top:
            # a regex literal can only follow an operator or an opener
            k = len(out) - 1
            while k >= 0 and out[k] in ' \t\n':
                k -= 1
            prev = out[k] if k >= 0 else '('
            if prev in '(,=:[!&|?;{}' or ''.join(out[-6:]).endswith('return'):
                i += 1
                while i < n and '/' != js[i]:
                    i += 2 if '\\' == js[i] else 1
                    if i < n and '[' == js[i - 1]:      # a class may hold '/'
                        while i < n and ']' != js[i]:
                            i += 1
                i += 1
                out.append(' ')
                continue
        if '/*' == two:
            i = js.find('*/', i + 2)
            i = n if -1 == i else i + 2
            continue
        if c in '"\'':
            i += 1
            while i < n and js[i] != c:
                i += 2 if '\\' == js[i] else 1
            i += 1
            out.append(' ')
            continue
        if '`' == c:
            stack.append(['tmpl'])
            out.append(' ')
            i += 1
            continue
        if 'sub' == top:
            if '{' == c:
                stack[-1][1] += 1
            elif '}' == c:
                if 0 == stack[-1][1]:
                    stack.pop()
                    out.append(' ')
                    i += 1
                    continue
                stack[-1][1] -= 1
        out.append(c)
        i += 1
    return ''.join(out)


def sources(path):
    """(markup, script) from webui/ — or from a single inlined page."""
    p = pathlib.Path(path)
    if p.is_dir():
        return ((p / 'index.html').read_text(encoding='utf-8'),
                (p / 'app.js').read_text(encoding='utf-8'))
    src = p.read_text(encoding='utf-8')
    return src, src[src.index('<script>'):] if '<script>' in src else ''


def main(path):
    html, src = sources(path)
    js = strip_noise(src)
    problems = []

    ids_html = set(re.findall(r'(?<![\w-])id="([\w-]+)"', html))
    ids_html |= set(re.findall(r"(?<![\w-])id=\\?['\"]?([\w-]+)", src))   # built in JS
    # scan the raw script: these lookups live inside string literals
    used_ids = set(re.findall(r"""\$\(['"]([\w-]+)['"]\)""", src))
    used_ids |= set(re.findall(r"""getElementById\(['"]([\w-]+)['"]\)""", src))
    used_ids |= set(re.findall(r"""querySelector(?:All)?\(['"]#([\w-]+)""", src))
    for used in sorted(used_ids):
        if used not in ids_html:
            problems.append("'%s' is looked up but no element has that id" % used)
    # every nav tab needs its section: showTab() builds "tab-<name>" at runtime
    for tab in re.findall(r'data-tab="([\w-]+)"', html):
        if 'tab-' + tab not in ids_html:
            problems.append('nav tab "%s" has no <section id="tab-%s">' % (tab, tab))
    # data-* keys written in templates must be the ones the handlers read
    written = set(re.findall(r'data-([a-z]+)=', src)) | set(re.findall(r'data-([a-z]+)=', html))
    read = set(re.findall(r'dataset\.([a-zA-Z]+)', src))
    read |= set(re.findall(r'\[data-([a-z]+)[\]=]', src))
    for key in sorted(read - written - {'tab'}):
        problems.append('data-%s is read but never written' % key)
    for key in sorted(written - read - {'tab', 'bv', 'feed', 'total', 'custom', 'zc'}):
        problems.append('data-%s is written but never read' % key)

    defined = set(re.findall(r'\bfunction\s+([\w$]+)', js))
    defined |= set(re.findall(r'([\w$]+)\s*\([^()]*\)\s*\{', js))   # methods
    defined |= set(re.findall(r'([\w$]+)\s*(?::|=(?!=))\s*(?:async\s*)?[\w$(]', js))
    for decl in re.findall(r'\b(?:const|let|var)\s+([^;\n]+)', js):
        for part in decl.split(','):
            name = part.split('=')[0].strip()
            if re.fullmatch(r'[\w$]+', name):
                defined.add(name)
    # parameters: a callback passed in is "defined" as far as this file goes
    for params in re.findall(r'\(([^()]*)\)\s*(?:=>|\{)', js):
        for part in params.split(','):
            name = part.split('=')[0].strip()
            if re.fullmatch(r'[\w$]+', name):
                defined.add(name)

    called = set(re.findall(r'(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(', js))
    for name in sorted(called - defined - BUILTINS - KEYWORDS):
        problems.append('%s() is called but never defined' % name)

    if problems:
        print('%s: %d problem(s)' % (path, len(problems)), file=sys.stderr)
        for p in problems:
            print('  - %s' % p, file=sys.stderr)
        return 1
    print('%s: ids and symbols resolve' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'webui'))
