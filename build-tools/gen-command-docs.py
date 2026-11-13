#!/usr/bin/env python3
"""Build src/command_docs.inc from the Redis 7.0.15 generated command table.

The 7.0.15 table uses struct layouts this tree does not have, so it is parsed
as text and emitted as flat tables of our own. ZRANK and ZREVRANK are then
overlaid with the Redis 7.2.5 JSON specs, which add the WITHSCORE argument
this port implements and 7.0.15 does not document.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / 'build-tools' / 'upstream-ref' / 'commands-7.0.15.c'
LOCAL = ROOT / 'build-tools' / 'local-commands.txt'
OUT = ROOT / 'src' / 'command_docs.inc'
OVERLAYS = [
    ROOT / 'build-tools' / 'upstream-ref' / 'zrank-7.2.5.json',
    ROOT / 'build-tools' / 'upstream-ref' / 'zrevrank-7.2.5.json',
]

GROUPS = {
    'BITMAP': 'bitmap', 'CLUSTER': 'cluster', 'CONNECTION': 'connection',
    'GENERIC': 'generic', 'GEO': 'geo', 'HASH': 'hash',
    'HYPERLOGLOG': 'hyperloglog', 'LIST': 'list', 'PUBSUB': 'pubsub',
    'SCRIPTING': 'scripting', 'SENTINEL': 'sentinel', 'SERVER': 'server',
    'SET': 'set', 'SORTED_SET': 'sorted_set', 'STREAM': 'stream',
    'STRING': 'string', 'TRANSACTIONS': 'transactions',
}

TYPES = {
    'ARG_TYPE_STRING': 'string', 'ARG_TYPE_INTEGER': 'integer',
    'ARG_TYPE_DOUBLE': 'double', 'ARG_TYPE_KEY': 'key',
    'ARG_TYPE_PATTERN': 'pattern', 'ARG_TYPE_UNIX_TIME': 'unix-time',
    'ARG_TYPE_PURE_TOKEN': 'pure-token', 'ARG_TYPE_ONEOF': 'oneof',
    'ARG_TYPE_BLOCK': 'block',
}

ARG_RE = re.compile(
    r'\{"([^"]*)",\s*(ARG_TYPE_\w+),\s*(-?\d+),\s*'
    r'(NULL|"[^"]*"),\s*(NULL|"[^"]*"),\s*(NULL|"[^"]*"),\s*'
    r'(CMD_ARG_[A-Z_|]+)'
    r'(?:,\s*\.subargs=(\w+))?', re.S)

ROW_RE = re.compile(
    r'\{"([a-z0-9_|-]+)","((?:[^"\\]|\\.)*)",(NULL|"(?:[^"\\]|\\.)*"),'
    r'"([0-9.]+)",CMD_DOC_\w+,(?:NULL|"[^"]*"),(?:NULL|"[^"]*"),'
    r'COMMAND_GROUP_(\w+),(\w+),[\w.]+,(NULL|\w+)([^\n]*)}', re.S)


def unquote(token):
    return None if token == 'NULL' else token[1:-1]


def c_string(text):
    out = ['"']
    for ch in text:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ord(ch) < 32 or ord(ch) > 126:
            raise SystemExit('non-ascii in generated string: ' + repr(text))
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def parse_arg_tables(src):
    tables = {}
    for match in re.finditer(
            r'struct redisCommandArg (\w+)\[\] = \{(.*?)\n\};', src, re.S):
        rows = []
        for row in ARG_RE.finditer(match.group(2)):
            rows.append({
                'name': row.group(1), 'type': TYPES[row.group(2)],
                'key_spec': int(row.group(3)), 'token': unquote(row.group(4)),
                'summary': unquote(row.group(5)), 'since': unquote(row.group(6)),
                'flags': row.group(7), 'subargs': row.group(8),
            })
        tables[match.group(1)] = rows
    return tables


def arg_count(tables, name):
    return sum(1 + (arg_count(tables, row['subargs']) if row['subargs'] else 0)
               for row in tables[name])


def emit_args(out, tables, name, lines, index):
    """Flatten one argument table into `lines`, children right after parent.

    Each row records its span, the number of rows it occupies including the
    arguments nested inside it, so the reader walks siblings by addition.
    """
    start = index[0]
    for row in tables[name]:
        slot = index[0]
        index[0] += 1
        lines.append(None)
        child = -1
        if row['subargs']:
            child = index[0]
            emit_args(out, tables, row['subargs'], lines, index)
        flags = []
        if 'OPTIONAL' in row['flags']:
            flags.append('CMDDOC_OPTIONAL')
        if 'MULTIPLE_TOKEN' in row['flags']:
            flags.append('CMDDOC_MULTIPLE_TOKEN')
        elif 'MULTIPLE' in row['flags']:
            flags.append('CMDDOC_MULTIPLE')
        lines[slot] = (
            '    {%d,%d,%d,%d,%s,%s,%s,%s,%s,%s},' % (
                slot, index[0] - slot, child, row['key_spec'],
                '|'.join(flags) if flags else '0',
                c_string(row['name']), c_string(row['type']),
                c_string(row['token']) if row['token'] else 'NULL',
                c_string(row['summary']) if row['summary'] else 'NULL',
                c_string(row['since']) if row['since'] else 'NULL'))
    return start, index[0] - start


def json_args(node, lines, index):
    """Same flattening for a 7.2 JSON argument list."""
    start = index[0]
    for arg in node:
        slot = index[0]
        index[0] += 1
        lines.append(None)
        child = -1
        if 'arguments' in arg:
            child = index[0]
            json_args(arg['arguments'], lines, index)
        flags = []
        if arg.get('optional'):
            flags.append('CMDDOC_OPTIONAL')
        if arg.get('multiple_token'):
            flags.append('CMDDOC_MULTIPLE_TOKEN')
        elif arg.get('multiple'):
            flags.append('CMDDOC_MULTIPLE')
        lines[slot] = (
            '    {%d,%d,%d,%d,%s,%s,%s,%s,%s,%s},' % (
                slot, index[0] - slot, child, arg.get('key_spec_index', -1),
                '|'.join(flags) if flags else '0',
                c_string(arg['name']), c_string(arg['type']),
                c_string(arg['token']) if arg.get('token') else 'NULL',
                c_string(arg['summary']) if arg.get('summary') else 'NULL',
                c_string(arg['since']) if arg.get('since') else 'NULL'))
    return start, index[0] - start


KEY_FLAGS = [
    ('CMD_KEY_RO', 'RO'), ('CMD_KEY_RW', 'RW'), ('CMD_KEY_OW', 'OW'),
    ('CMD_KEY_RM', 'RM'), ('CMD_KEY_ACCESS', 'access'),
    ('CMD_KEY_UPDATE', 'update'), ('CMD_KEY_INSERT', 'insert'),
    ('CMD_KEY_DELETE', 'delete'), ('CMD_KEY_NOT_KEY', 'not_key'),
    ('CMD_KEY_INCOMPLETE', 'incomplete'),
    ('CMD_KEY_VARIABLE_FLAGS', 'variable_flags'),
]

SPEC_RE = re.compile(
    r'CMD_KEY_[A-Z_|]+,KSPEC_BS_(\w+),'
    r'(?:\.bs\.index=\{(-?\d+)\}|\.bs\.keyword=\{"([^"]*)",(-?\d+)\}|\{\{0\}\}),'
    r'KSPEC_FK_(\w+),'
    r'(?:\.fk\.range=\{(-?\d+),(-?\d+),(-?\d+)\}'
    r'|.fk\.keynum=\{(-?\d+),(-?\d+),(-?\d+)\}|\{\{0\}\})')


def parse_specs(rest):
    """Key specs that name their keys by position. Keyword and unknown specs
     * are reported separately because this tree cannot resolve them."""
    specs, skipped = [], []
    for m in SPEC_RE.finditer(rest):
        search, pos, keyword, kwarg, find = m.group(1), m.group(2), \
            m.group(3), m.group(4), m.group(5)
        flags = [name for macro, name in KEY_FLAGS if macro in m.group(0)]
        if search == 'INDEX' and find == 'RANGE':
            last, step, limit = (int(m.group(i)) for i in (6, 7, 8))
            specs.append((int(pos), last, step, limit, 0, flags))
        elif search == 'INDEX' and find == 'KEYNUM':
            idx, first, step = (int(m.group(i)) for i in (9, 10, 11))
            specs.append((int(pos) + idx, first, step, 0, 1, flags))
        else:
            skipped.append(search + '/' + find)
    return specs, skipped


def main():
    src = TABLE.read_text(encoding='utf-8')
    local = set(LOCAL.read_text(encoding='ascii').split())
    tables = parse_arg_tables(src)

    # Subcommands live in their own arrays above the top-level table and
    # reuse names such as "get". Only the top-level table is addressable.
    top_level = src[src.index('struct redisCommand redisCommandTable[]'):]
    rows = []
    for match in ROW_RE.finditer(top_level):
        name, summary, complexity, since, group, history, proc, rest = \
            match.groups()
        if '|' in name or name not in local:
            continue
        ident = name.upper().replace('-', '_')
        if complexity != 'NULL':
            complexity = complexity[1:-1]
        specs, skipped = parse_specs(rest)
        rows.append({
            'name': name, 'summary': summary,
            'complexity': None if complexity == 'NULL' else complexity,
            'since': since, 'group': GROUPS[group], 'history': history,
            'proc': proc, 'args': ident + '_Args',
            'specs': specs, 'specs_skipped': skipped,
        })
    # Every name in the local command table is a real command here, even
    # when upstream implements it as a subcommand (XREADGROUP) or an alias
    # (HMSET). Drop only the upstream subcommands this tree does not have.
    rows = [row for row in rows if row['name'] in local]

    histories = {}
    for match in re.finditer(
            r'commandHistory (\w+)\[\] = \{(.*?)\n\};', src, re.S):
        entries = re.findall(r'\{"([^"]*)","((?:[^"\\]|\\.)*)"\}',
                             match.group(2))
        histories[match.group(1)] = [(v, t.encode().decode('unicode_escape'))
                                     for v, t in entries]

    # 7.2.5 overlay: WITHSCORE on ZRANK / ZREVRANK.
    for path in OVERLAYS:
        spec = json.loads(path.read_text(encoding='utf-8'))
        for name, doc in spec.items():
            row = next(r for r in rows if r['name'] == name.lower())
            row['summary'] = doc['summary']
            row['complexity'] = doc['complexity']
            row['since'] = doc['since']
            row['history_inline'] = [(v, t) for v, t in doc.get('history', [])]
            row['args_inline'] = doc.get('arguments', [])

    arg_lines, hist_lines = [], []
    arg_index, hist_index = [0], [0]
    for row in rows:
        if 'args_inline' in row:
            row['arg_at'], row['arg_n'] = json_args(
                row['args_inline'], arg_lines, arg_index)
        elif row['args'] in tables:
            row['arg_at'], row['arg_n'] = emit_args(
                None, tables, row['args'], arg_lines, arg_index)
        else:
            row['arg_at'], row['arg_n'] = -1, 0
        entries = row.get('history_inline', histories.get(row['history'], []))
        if entries:
            row['hist_at'] = hist_index[0]
            row['hist_n'] = len(entries)
            for version, text in entries:
                hist_lines.append('    {%d,%s,%s},' % (
                    hist_index[0], c_string(version), c_string(text)))
                hist_index[0] += 1
        else:
            row['hist_at'], row['hist_n'] = -1, 0

    out = [
        '/* Generated by build-tools/gen-command-docs.py. Do not edit. */',
        '/* Sources: Redis 7.0.15 src/commands.c, plus the 7.2.5 ZRANK and',
        ' * ZREVRANK specs for the WITHSCORE argument. */', '',
        'typedef struct cmdDocArg {',
        '    int index; int span; int child; int key_spec; int flags;',
        '    const char *name; const char *type;',
        '    const char *token; const char *summary; const char *since;',
        '} cmdDocArg;', '',
        'typedef struct cmdDocHist {',
        '    int index; const char *since; const char *text;',
        '} cmdDocHist;', '',
        'typedef struct cmdDocKeySpec {',
        '    int first; int last; int step; int limit; int keynum;',
        '    const char *flags[8];',
        '} cmdDocKeySpec;', '',
        'typedef struct cmdDocEntry {',
        '    const char *name; const char *summary; const char *complexity;',
        '    const char *since; const char *group;',
        '    int arg_at; int arg_n; int hist_at; int hist_n;',
        '    int spec_at; int spec_n;',
        '} cmdDocEntry;', '',
        '#define CMDDOC_OPTIONAL 1',
        '#define CMDDOC_MULTIPLE 2',
        '#define CMDDOC_MULTIPLE_TOKEN 4', '',
        'static const cmdDocArg cmdDocArgs[] = {']
    # The flattened form stores child offsets, so the struct field order in
    # the brace list above must match emit_args. Reorder to named safety by
    # emitting positional in the declared order.
    out += arg_lines
    out += ['    {0}', '};', '', 'static const cmdDocHist cmdDocHistories[] = {']
    out += hist_lines
    spec_lines, spec_at = [], 0
    for row in rows:
        if row['specs']:
            row['spec_at'] = spec_at
            row['spec_n'] = len(row['specs'])
            for first, last, step, limit, keynum, flags in row['specs']:
                names = ','.join(c_string(f) for f in flags)
                spec_lines.append(
                    '    {%d,%d,%d,%d,%d,{%s,NULL}},' % (
                        first, last, step, limit, keynum, names))
                spec_at += 1
        else:
            row['spec_at'], row['spec_n'] = -1, 0

    out += ['    {0}', '};', '',
            'static const cmdDocKeySpec cmdDocKeySpecs[] = {']
    out += spec_lines
    out += ['    {0}', '};', '',
            '/* Kept sorted by name for bsearch. */',
            'static const cmdDocEntry cmdDocTable[] = {']
    for row in sorted(rows, key=lambda r: r['name']):
        out.append(
            '    {%s,%s,%s,%s,%s,%d,%d,%d,%d,%d,%d},' % (
                c_string(row['name']), c_string(row['summary']),
                c_string(row['complexity']) if row['complexity'] else 'NULL',
                c_string(row['since']),
                c_string(row['group']), row['arg_at'], row['arg_n'],
                row['hist_at'], row['hist_n'],
                row['spec_at'], row['spec_n']))
    out += ['    {0}', '};', '']
    text = '\n'.join(line for line in out if line is not None)
    OUT.write_bytes(text.replace('\n', '\r\n').encode('ascii'))
    # These have no upstream docs: internal handshake commands, LOLWUT, and
    # WAITAOF which arrived after 7.2. RESTORE-ASKING is cluster-only.
    expected = {'post', 'host:', 'psync', 'sync', 'lolwut', 'waitaof',
                'restore-asking'}
    missing = sorted(local - {r['name'] for r in rows} - expected)
    if missing:
        raise SystemExit('local commands lost their docs: ' + ' '.join(missing))
    print('commands %d, args %d, history %d' % (
        len(rows), arg_index[0], hist_index[0]))


if __name__ == '__main__':
    sys.exit(main())
