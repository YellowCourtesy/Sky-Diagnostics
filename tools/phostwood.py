#!/usr/bin/env python3
"""Data of Phostwood's Crash Log Analyzer for the crash report.

Source: https://github.com/Phostwood/crash-analyzer (GPL-3.0, by Phostwood).
Only the data tables are used, not the code of the analyzer. This script does
not run any code of that repository: it reads the JavaScript files as text.

Two commands:

  phostwood.py import <analyzer folder> <json file>
      Reads the data tables of a copy of the analyzer repository and writes
      them as JSON (data/phostwood.json in this repository). The build
      workflow runs this with the latest analyzer before each build.

  phostwood.py inc <json file> <inc file>
      Writes the C++ tables (generated/PhostwoodData.inc) from the JSON file.
      CMake runs this when it configures the build.
"""

import datetime
import json
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# A reader for the part of JavaScript that the data files use: object and
# array literals, strings, numbers, true/false/null, comments. Anything else
# is an error, and the build then uses the JSON file in the repository.


class ParseError(Exception):
    pass


class Reader:
    def __init__(self, text, pos=0):
        self.t = text
        self.i = pos

    def skip(self):
        t = self.t
        while self.i < len(t):
            c = t[self.i]
            if c in ' \t\r\n':
                self.i += 1
            elif t.startswith('//', self.i):
                e = t.find('\n', self.i)
                self.i = len(t) if e < 0 else e + 1
            elif t.startswith('/*', self.i):
                e = t.find('*/', self.i + 2)
                if e < 0:
                    raise ParseError('comment with no end')
                self.i = e + 2
            else:
                break

    def peek(self):
        self.skip()
        return self.t[self.i] if self.i < len(self.t) else ''

    def expect(self, c):
        if self.peek() != c:
            raise ParseError(f'expected {c!r} at {self.where()}')
        self.i += 1

    def where(self):
        line = self.t.count('\n', 0, self.i) + 1
        return f'line {line}'

    def value(self):
        c = self.peek()
        if c == '{':
            return self.obj()
        if c == '[':
            return self.arr()
        if c in '\'"`':
            return self.string()
        m = re.compile(r'-?\d+(\.\d+)?').match(self.t, self.i)
        if m:
            self.i = m.end()
            return float(m.group(0)) if m.group(1) else int(m.group(0))
        for word, val in (('true', True), ('false', False), ('null', None), ('undefined', None)):
            if self.t.startswith(word, self.i):
                self.i += len(word)
                return val
        raise ParseError(f'not a data value at {self.where()}')

    def string(self):
        q = self.t[self.i]
        self.i += 1
        out = []
        t = self.t
        while self.i < len(t):
            c = t[self.i]
            if c == '\\':
                n = t[self.i + 1]
                out.append({'n': '\n', 't': '\t', 'r': ''}.get(n, n))
                self.i += 2
                continue
            if q == '`' and t.startswith('${', self.i):
                raise ParseError(f'template with code at {self.where()}')
            if c == q:
                self.i += 1
                return ''.join(out)
            out.append(c)
            self.i += 1
        raise ParseError('string with no end')

    def key(self):
        c = self.peek()
        if c in '\'"':
            return self.string()
        m = re.compile(r'[A-Za-z_$][A-Za-z0-9_$]*').match(self.t, self.i)
        if not m:
            raise ParseError(f'not a key at {self.where()}')
        self.i = m.end()
        return m.group(0)

    def obj(self):
        self.expect('{')
        out = {}
        while True:
            if self.peek() == '}':
                self.i += 1
                return out
            k = self.key()
            self.expect(':')
            out[k] = self.value()
            if self.peek() == ',':
                self.i += 1

    def arr(self):
        self.expect('[')
        out = []
        while True:
            if self.peek() == ']':
                self.i += 1
                return out
            out.append(self.value())
            if self.peek() == ',':
                self.i += 1


def literal_after(text, pattern):
    """The literal that starts after the regular expression pattern."""
    m = re.search(pattern, text)
    if not m:
        raise ParseError(f'{pattern!r} not found')
    return Reader(text, m.end()).value()


def plain(text):
    """Text with no HTML: tags removed, entities and spaces made simple."""
    if not isinstance(text, str):
        return ''
    text = re.sub(r'<br\s*/?>', ' ', text, flags=re.I)
    text = re.sub(r'<[^>]+>', '', text)
    for a, b in (('&nbsp;', ' '), ('&lt;', '<'), ('&gt;', '>'), ('&quot;', '"'), ('&#39;', "'"), ('&amp;', '&')):
        text = text.replace(a, b)
    return re.sub(r'\s+', ' ', text).strip()


# ---------------------------------------------------------------------------


# Conditions that the analyzer has in its code, not in its data tables, so the
# import cannot read them. Each one has the date it was written here: check it
# against the analyzer when the data changes.
#   requires_module  the group counts only if this DLL is loaded
#   requires_text    the group counts only if this text is in the crash place
#                    (call stack, registers, stack)
#   possible_only    the analyzer shows the group as "possible" only
GROUP_CONDITIONS = {
    'dragonsEyeMinimapIssues': {'requires_module': 'DragonsEyeMinimap.dll', 'added': '2026-11-18'},
    'lotdKaragasTowerDoorCrashIssues': {'requires_text': 'LegacyoftheDragonborn', 'added': '2026-11-18'},
    'enbShaderLightingIssues': {'possible_only': True, 'added': '2026-11-18'},
}


def do_import(folder, out_path):
    js = os.path.join(folder, 'js')

    def read(name):
        with open(os.path.join(js, name), encoding='utf-8') as f:
            return f.read()

    # Crash signs: groups of names that are in a crash log when a problem is
    # the cause. Only the groups and lists that the analyzer code uses (some
    # lists in the file are not used, and are too general).
    indicators = literal_after(read('crashIndicatorsMap.js'), r'const\s+crashIndicators\s*=\s*')
    used = set()
    for name in os.listdir(js):
        if name.endswith('.js') and name != 'crashIndicatorsMap.js' and ' - ' not in name:
            used.update(re.findall(r'crashIndicators\.([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)', read(name)))
    groups = []
    for key, g in indicators.items():
        if not isinstance(g, dict):
            continue
        items, hex_codes = [], []
        for field, entries in g.items():
            if (key, field) not in used or not isinstance(entries, list):
                continue
            for e in entries:
                if not isinstance(e, dict):
                    continue
                if field == 'hexCodes':
                    if isinstance(e.get('hexCode'), str) and e['hexCode'].strip():
                        hex_codes.append({'hex': e['hexCode'].strip(), 'what': plain(e.get('description', ''))})
                    continue
                text = next((e[k] for k in ('code', 'name', 'file') if isinstance(e.get(k), str) and e[k].strip()), None)
                if text:
                    items.append({'text': text, 'what': plain(e.get('description', ''))})
        if items or hex_codes:
            group = {'key': key, 'items': items, 'hex': hex_codes}
            group.update({k: v for k, v in GROUP_CONDITIONS.get(key, {}).items()})
            groups.append(group)

    # DLL versions that do not work with newer game versions.
    compat_src = literal_after(read('dllCompatibleSkyrimVersionsMap.js'), r'const\s+dllCompatibleSkyrimVersionsMap\s*=\s*')
    compat = []
    for dll, versions in compat_src.items():
        if not isinstance(versions, dict) or not versions:
            continue
        newest = max(versions, key=version_key)
        d = versions[newest]
        compat.append({
            'dll': dll,
            'version': newest,
            'maxGame': d.get('maxSkyrim') or '',
            'ignoreBelow': d.get('ignoreVersionsBelow') or '',
            'recommended': d.get('recommendedVersion') or '',
            'mod': plain(d.get('modName', '')),
            'url': d.get('url') or '',
            'note': plain(d.get('note', '')),
        })

    # Short explanations of file names (DLLs, plugins).
    explain_src = literal_after(read('explainersMap.js'), r'new\s+Map\s*\(\s*')
    explain = []
    for e in explain_src:
        if isinstance(e, list) and len(e) == 2 and isinstance(e[0], str):
            explain.append({'file': e[0].lower(), 'what': plain(e[1]).strip('() ')})

    def signatures(name, pattern):
        src = literal_after(read(name), pattern)
        return [{'product': k, 'files': [f for f in v if isinstance(f, str)]} for k, v in src.items() if isinstance(v, list)]

    antivirus = signatures('antivirusSignatures.js', r'const\s+antivirusSignatures\s*=\s*')
    overlays = signatures('overlaySignatures.js', r'overlaySignatures\s*=\s*')

    commit, date = '', ''
    try:
        commit = subprocess.run(['git', '-C', folder, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        date = subprocess.run(['git', '-C', folder, 'log', '-1', '--format=%cs'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        pass

    if len(groups) < 5 or len(explain) < 50:
        raise ParseError(f'too little data ({len(groups)} groups, {len(explain)} explanations): the format of the analyzer changed')

    data = {
        'source': 'https://github.com/Phostwood/crash-analyzer',
        'license': 'GPL-3.0',
        'commit': commit,
        'date': date,
        'imported': datetime.date.today().isoformat(),
        'groups': groups,
        'dllVersions': compat,
        'explanations': explain,
        'antivirus': antivirus,
        'overlays': overlays,
    }
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write('\n')
    print(f'Phostwood data {commit[:7]} ({date}): {len(groups)} sign groups, {len(compat)} DLL version rules, '
          f'{len(explain)} explanations, {len(antivirus)} antivirus and {len(overlays)} overlay products')


def version_key(v):
    return [int(x) if x.isdigit() else 0 for x in re.split(r'[.\-]', str(v))]


def c_str(s):
    s = str(s).encode('utf-8')
    out = []
    for b in s:
        ch = chr(b)
        if ch == '\\' or ch == '"':
            out.append('\\' + ch)
        elif 32 <= b < 127 and ch != '?':
            out.append(ch)
        else:
            out.append(f'\\{b:03o}')
    return '"' + ''.join(out) + '"'


def c_version(v):
    parts = version_key(v)[:4] if v else []
    parts += [0] * (4 - len(parts))
    return '{ ' + ', '.join(str(min(p, 65535)) for p in parts) + ' }'


def do_inc(json_path, out_path):
    with open(json_path, encoding='utf-8') as f:
        d = json.load(f)
    lines = ['// Made by tools/phostwood.py from data/phostwood.json. Do not change.',
             '// Data of Phostwood\'s Crash Log Analyzer (GPL-3.0): https://github.com/Phostwood/crash-analyzer',
             f'static constexpr const char* kPhostwoodCommit = {c_str(d.get("commit", "")[:7])};',
             f'static constexpr const char* kPhostwoodDate = {c_str(d.get("date", ""))};', '']

    items, groups = [], []
    for g in d['groups']:
        first = len(items)
        for it in g['items']:
            items.append(f'\t{{ {c_str(it["text"])}, {c_str(it["what"])} }},')
        count = len(items) - first
        for h in g['hex']:
            items.append(f'\t{{ {c_str(h["hex"])}, {c_str(h["what"])} }},')
        groups.append(f'\t{{ {c_str(g["key"])}, {first}, {count}, {len(g["hex"])}, {c_str(g.get("requires_module", ""))}, '
                      f'{c_str(g.get("requires_text", ""))}, {"true" if g.get("possible_only") else "false"}, {c_str(g.get("added", ""))} }},')
    lines += ['static constexpr PhostwoodItem kPhostwoodItems[] = {'] + (items or ['\t{ "", "" },']) + ['};']
    lines += ['static constexpr PhostwoodGroup kPhostwoodGroups[] = {'] + (groups or ['\t{ "", 0, 0, 0, "", "", false, "" },']) + ['};', '']

    rows = [f'\t{{ {c_str(c["dll"])}, {c_version(c["version"])}, {c_version(c["maxGame"])}, {c_version(c["ignoreBelow"])}, '
            f'{c_str(c["recommended"])}, {c_str(c["mod"])}, {c_str(c["url"])}, {c_str(c["note"])} }},' for c in d['dllVersions']]
    lines += ['static constexpr PhostwoodDllVersion kPhostwoodDllVersions[] = {'] + (rows or ['\t{},']) + ['};', '']

    rows = [f'\t{{ {c_str(e["file"])}, {c_str(e["what"])} }},' for e in d['explanations']]
    lines += ['static constexpr PhostwoodExplanation kPhostwoodExplanations[] = {'] + (rows or ['\t{ "", "" },']) + ['};', '']

    for name, key in (('kPhostwoodAntivirus', 'antivirus'), ('kPhostwoodOverlays', 'overlays')):
        rows = [f'\t{{ {c_str(p["product"])}, {c_str(f)} }},' for p in d[key] for f in p['files']]
        lines += [f'static constexpr PhostwoodProductFile {name}[] = {{'] + (rows or ['\t{ "", "" },']) + ['};', '']

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'import':
        try:
            do_import(sys.argv[2], sys.argv[3])
        except (ParseError, OSError, KeyError, IndexError) as e:
            print(f'Phostwood data not imported: {e}', file=sys.stderr)
            return 1
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == 'inc':
        do_inc(sys.argv[2], sys.argv[3])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
