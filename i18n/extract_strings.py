#!/usr/bin/env python3
"""Unified i18n string extractor for Snes9x.

Collects every translatable string from the win32, Qt and GTK front-ends and
appends any that is missing to i18n/snes9x.pot, then gives every i18n/<lang>.po
each snes9x.pot entry it lacks, with an empty msgstr. Existing entries and
translations are never rewritten.

  win32  rsrc/snes9x.rc menus + dialog controls, wlanguage.h macros, _L("...")
  Qt     tr(), QT_TR_NOOP(), QT_TRANSLATE_NOOP(), translate(), .ui <string>
  GTK    _(), N_(), gettext(), maybegettext(), ngettext(), snes9x.ui
  all    the tables in TABLES, which reach _L()/tr()/_() through a variable

win32 and Qt look strings up by their normalized key (normalize_key); GTK uses
gettext, so its msgids are kept exact.

Usage:  python3 i18n/extract_strings.py [--dry-run]
"""

import glob
import html
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
RC = os.path.join(ROOT, "win32", "rsrc", "snes9x.rc")
WLANG = os.path.join(ROOT, "win32", "wlanguage.h")
POT = os.path.join(SCRIPT_DIR, "snes9x.pot")

TEXT_CONTROLS = (
    "LTEXT", "RTEXT", "CTEXT", "PUSHBUTTON", "DEFPUSHBUTTON",
    "GROUPBOX", "AUTOCHECKBOX", "AUTORADIOBUTTON", "CHECKBOX",
    "RADIOBUTTON", "CONTROL",
)

C_ESCAPES = {"t": "\t", "n": "\n", "r": "\r", "a": "\a", "b": "\b",
             "f": "\f", "v": "\v", "0": "\0", "\\": "\\", '"': '"', "'": "'"}

C_STR = r'"((?:[^"\\]|\\.)*)"'
WIDE_STR = r'L' + C_STR
TEXT_STR = r'TEXT\(\s*' + C_STR + r'\s*\)'

# Strings that reach _L() / tr() / _() through a variable: (front-ends, file,
# block, string), front-ends being w(in32), q(t), g(tk). The string regex runs
# inside each block match, or over the whole file when block is None; each of
# its groups is a string.
TABLES = [
    ("w", "win32/wlua.cpp", None, r'\b(?:popup|item)\([^;]*?' + WIDE_STR),
    ("w", "win32/wlua.cpp", r'\b(?:scopes|labels|panels|field_labels)\[[^\]]*\]\s*=\s*\{[^}]*\}', WIDE_STR),
    ("w", "win32/wlua.cpp", r'\bcolumns\(\s*[^,]+,\s*\{.*?\}\s*\);', WIDE_STR),
    ("w", "win32/wsnes9x.cpp", r'\bkBiosChipModes\[\]\s*=\s*\{.*?\};', TEXT_STR),
    ("wqg", "biosmanager.cpp", r'\bkFamilies\[[^\]]*\]\s*=\s*\{.*?\n\};', r'\{\s*' + C_STR),
    ("w", "biosmanager.cpp", r'\bkSlots\[[^\]]*\]\s*=\s*\{.*?\n\};', r'\{\s*"[^"]*",\s*' + C_STR),
    ("qg", "biosmanager.cpp", r'\bkSlots\[[^\]]*\]\s*=\s*\{.*?\n\};',
     r'\{\s*"[^"]*",\s*"[^"]*",\s*\w+,\s*[^,]+,\s*' + C_STR),
    ("wqg", "biosmanager.cpp", r'\bS9xBiosGroupName\s*\([^)]*\)\s*\{.*?\n\}', C_STR),
    ("wqg", "biosmanager.cpp", r'\bS9xGetBiosNesting\s*\([^)]*\)\s*\{.*?\n\}', C_STR),
    ("q", "memmap.cpp", r'\bS9xGBBootPolicyName\s*\([^)]*\)\s*\{.*?\n\}', C_STR),
    ("qg", "common/video/gb_ppu_viewer.cpp",
     r'\bconst char \*(?:palette_mode_name|map_name|tile_data_name|map_background_name)\s*\([^)]*\)\s*\{.*?\n\}', C_STR),
    ("qg", "common/video/ppu_viewer.cpp",
     r'\bconst char \*(?:source_name|bit_depth_name|tile_layout_name|viewer_bg_name|tilemap_background_name)'
     r'\s*\([^)]*\)\s*\{.*?\n\}', C_STR),
    ("q", "lua/lua_examples.cpp", None, r'^\s*\{\s*' + C_STR + r',\s*' + C_STR + r',\s*"[^"]*\.lua"'),
]

# Variable call sites whose strings TABLES, wlanguage.h or a marker (N_,
# QT_TR_NOOP...) already cover: file -> first identifiers of the argument. Any
# other site is reported so a TABLES row gets added.
DYNAMIC_OK = {
    "win32/wlua.cpp": {"scopes", "label", "labels", "panels", "column", "field_labels"},
    "win32/wsnes9x.cpp": {"m", "kBiosChipModes", "Utf8ToWide", "item"},
    "qt/src/BiosManagerDialog.cpp": {"const", "text", "m", "S9xGetBiosFamily", "S9xBiosGroupName", "n", "note"},
    "qt/src/EmuPoTranslator.cpp": {"const"},
    "qt/src/EmuPoTranslator.hpp": {"const"},
    "qt/src/EmuMainWindow.cpp": {"sfcbox_keyswitch_names", "game_only", "display_names", "S9xGBBootPolicyName",
                                 "toggle", "gb_layer_labels"},
    "qt/src/GBViewerWindows.cpp": {"gv", "pv"},
    "qt/src/LuaScriptWindow.cpp": {"example", "labels", "scopes"},
    "qt/src/SpriteViewerWindow.cpp": {"rate", "name"},
    "qt/src/TileViewerWindow.cpp": {"pv", "base_labels"},
    "qt/src/TilemapViewerWindow.cpp": {"pv"},
    "gtk/src/gtk_binding.cpp": {"String"},
    "gtk/src/gtk_bios_manager.cpp": {"m", "note", "S9xGetBiosFamily", "S9xBiosGroupName", "n"},
    "gtk/src/gtk_gb_viewer.cpp": {"gv", "pv"},
    "gtk/src/gtk_s9xwindow.cpp": {"game_only", "display_names"},
    "gtk/src/gtk_sprite_viewer.cpp": {"rate", "name"},
    "gtk/src/gtk_tile_viewer.cpp": {"pv"},
    "gtk/src/gtk_tilemap_viewer.cpp": {"pv"},
}

# Translation calls: name -> (msgctxt arg, msgid arg, msgid_plural arg).
QT_CALLS = {"tr": (None, 0, None), "QT_TR_NOOP": (None, 0, None),
            "QT_TRANSLATE_NOOP": (None, 1, None), "translate": (None, 1, None)}
GTK_CALLS = {"_": (None, 0, None), "N_": (None, 0, None), "gettext": (None, 0, None),
             "maybegettext": (None, 0, None), "ngettext": (None, 0, 1),
             "C_": (0, 1, None), "pgettext": (0, 1, None)}


def unescape_c(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append(C_ESCAPES.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def unescape_rc(s):
    return unescape_c(s.replace('""', '"'))


def normalize_key(s):
    head = s.split("\t", 1)[0].rstrip()
    if head.endswith("…"):
        head = head[:-1].rstrip()
    else:
        m = re.search(r"\.{3,}$", head)
        if m:
            head = head[:m.start()].rstrip()
    head = head.replace("&&", "\x00").replace("&", "").replace("\x00", "&")
    return head.strip()


def keep(key):
    if not key:
        return False
    if not re.search(r"[A-Za-z]", key):
        return False
    if re.fullmatch(r"%[\-0-9.]*[A-Za-z]", key):
        return False
    return True


def read(path):
    return open(path, encoding="utf-8", errors="replace").read()


def rel_of(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def warn_dynamic(rel, src, pos, ident):
    if ident not in DYNAMIC_OK.get(rel, ()):
        print("warning: %s:%d: translated variable %s... has no TABLES row" % (rel, line_of(src, pos), ident))


LITERAL_RE = re.compile(r'(?:u8|[LuU])?(?:R"([^(\s"]*)\((.*?)\)\1"|"((?:[^"\\\n]|\\.)*)")', re.S)


def call_args(src, i):
    """Arguments of the call whose '(' ends just before i: each is its string
    literal (adjacent ones joined), or None when it is anything else."""
    args, parts, literal, depth, n = [], [], True, 0, len(src)
    while i < n:
        c = src[i]
        if c.isspace():
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i)
            i = n if j < 0 else j + 2
            continue
        m = LITERAL_RE.match(src, i)
        if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
            if depth == 0:
                parts.append(m.group(2) if m.group(2) is not None else unescape_c(m.group(3)))
            i = m.end()
            continue
        if c == "'":
            m = re.compile(r"'(?:[^'\\]|\\.)*'").match(src, i)
            i = m.end() if m else i + 1
            literal = literal and depth > 0
            continue
        if c in ")]}" and depth == 0:
            args.append("".join(parts) if literal and parts else None)
            return args
        if c == "," and depth == 0:
            args.append("".join(parts) if literal and parts else None)
            parts, literal = [], True
        elif c in "([{":
            depth += 1
            literal = False
        elif c in ")]}":
            depth -= 1
        else:
            literal = literal and depth > 0
        i += 1
    return args


def scan_calls(path, calls):
    """(ctx, msgid, plural) for each translation call with literal arguments."""
    src, rel = read(path), rel_of(path)
    names = "|".join(re.escape(k) for k in sorted(calls, key=len, reverse=True))
    found = []
    for m in re.finditer(r'(?<![\w:.>])(?:\w+::)?(' + names + r')\s*\(', src):
        ctx_arg, id_arg, plural_arg = calls[m.group(1)]
        args = call_args(src, m.end())
        msgid = args[id_arg] if id_arg < len(args) else None
        if msgid is None:
            ident = re.match(r'\s*(?:"[^"]*"\s*,\s*)?([A-Za-z_]\w*)', src[m.end():])
            warn_dynamic(rel, src, m.start(), ident.group(1) if ident else "?")
            continue
        ctx = args[ctx_arg] if ctx_arg is not None and ctx_arg < len(args) else None
        plural = args[plural_arg] if plural_arg is not None and plural_arg < len(args) else None
        found.append((ctx, msgid, plural))
    return found


def table_strings(fronts):
    """(string, file) for each TABLES row serving any of the given front-ends."""
    out = []
    for rows_fronts, rel, block, string in TABLES:
        if not set(rows_fronts) & set(fronts):
            continue
        src = read(os.path.join(ROOT, rel))
        blocks = [m.group(0) for m in re.finditer(block, src, re.S)] if block else [src]
        found = 0
        for b in blocks:
            for m in re.finditer(string, b, re.M):
                for g in m.groups():
                    if g is not None:
                        out.append((unescape_c(g), rel))
                        found += 1
        if not found:
            print("warning: TABLES row for %s matched nothing: %s" % (rel, block or string))
    return out


def extract_keys():
    """win32 + Qt normalized keys: key -> source file."""
    keys = {}

    def add(raw, src):
        key = normalize_key(raw)
        if keep(key) and key not in keys:
            keys[key] = src

    rc_text = read(RC)
    rc_str = r'"((?:[^"]|"")*)"'
    for m in re.finditer(r'(?:MENUITEM|POPUP)\s+' + rc_str, rc_text):
        if m.group(1) and m.group(1) != "SEPARATOR":
            add(unescape_rc(m.group(1)), "win32/rsrc/snes9x.rc")
    for m in re.finditer(r'\bCAPTION\s+' + rc_str, rc_text):
        add(unescape_rc(m.group(1)), "win32/rsrc/snes9x.rc")
    ctrl_re = re.compile(r"^\s*(" + "|".join(TEXT_CONTROLS) + r")\s+" + rc_str, re.M)
    for m in ctrl_re.finditer(rc_text):
        if m.group(2):
            add(unescape_rc(m.group(2)), "win32/rsrc/snes9x.rc")

    wl_text = read(WLANG)
    for m in re.finditer(r'TEXT\(\s*"((?:[^"\\]|\\.)*)"\s*\)', wl_text):
        add(unescape_c(m.group(1)), "win32/wlanguage.h")

    sources = sorted(glob.glob(os.path.join(ROOT, "win32", "*.cpp")) +
                     glob.glob(os.path.join(ROOT, "win32", "*.h")))
    for path in sources:
        src = read(path)
        rel = rel_of(path)
        for m in re.finditer(r'_L\(\s*(?:_T|TEXT)\s*\(\s*"((?:[^"\\]|\\.)*)"', src):
            add(unescape_c(m.group(1)), rel)
        for m in re.finditer(r'_L\(\s*L?"((?:[^"\\]|\\.)*)"', src):
            add(unescape_c(m.group(1)), rel)
        if os.path.basename(path).startswith("wlocale."):
            continue
        for m in re.finditer(r'\b_L\(\s*(?!(?:_T|TEXT)\s*\(|L?")([A-Za-z_]\w*)?', src):
            warn_dynamic(rel, src, m.start(), m.group(1) or "?")

    qt_sources = sorted(glob.glob(os.path.join(ROOT, "qt", "src", "*.cpp")) +
                        glob.glob(os.path.join(ROOT, "qt", "src", "*.hpp")) +
                        glob.glob(os.path.join(ROOT, "qt", "src", "*.h")))
    for path in qt_sources:
        for _ctx, msgid, _plural in scan_calls(path, QT_CALLS):
            add(msgid, rel_of(path))
    for path in sorted(glob.glob(os.path.join(ROOT, "qt", "src", "*.ui"))):
        src = read(path)
        for m in re.finditer(r'<string(?![^>]*notr="true")(?:\s+[^>]*)?>([^<]*)</string>', src):
            add(html.unescape(m.group(1)), rel_of(path))

    for raw, rel in table_strings("wq"):
        add(raw, rel)
    return keys


def extract_gtk():
    """GTK exact entries: (ctx, msgid) -> {plural, ref}."""
    entries = {}

    def add(ctx, msgid, plural, ref):
        if msgid and (ctx, msgid) not in entries:
            entries[(ctx, msgid)] = {"plural": plural, "ref": ref}

    for path in sorted(glob.glob(os.path.join(ROOT, "gtk", "src", "*.cpp")) +
                       glob.glob(os.path.join(ROOT, "gtk", "src", "*.h"))):
        for ctx, msgid, plural in scan_calls(path, GTK_CALLS):
            add(ctx, msgid, plural, rel_of(path))
    for path in sorted(glob.glob(os.path.join(ROOT, "gtk", "src", "*.ui"))):
        src = read(path)
        for m in re.finditer(r'<(property|col)\b([^>]*\btranslatable="yes"[^>]*)(?<!/)>([^<]*)</\1>', src):
            ctx = re.search(r'\bcontext="([^"]*)"', m.group(2))
            add(html.unescape(ctx.group(1)) if ctx else None, html.unescape(m.group(3)), None, rel_of(path))
    for raw, rel in table_strings("g"):
        add(None, raw, None, rel)
    return entries


PO_ESC = [("\\", "\\\\"), ('"', '\\"'), ("\t", "\\t"), ("\r", "\\r"), ("\n", "\\n")]


def po_escape(s):
    for a, b in PO_ESC:
        s = s.replace(a, b)
    return s


def po_unescape(s):
    out, i = [], 0
    rev = {"t": "\t", "n": "\n", "r": "\r", '"': '"', "\\": "\\"}
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append(rev.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_po_token(tok):
    tok = tok.strip()
    if tok.startswith('"') and tok.endswith('"'):
        return po_unescape(tok[1:-1])
    return ""


def parse_entries(path):
    """Each non-header entry as {ctx, id, plural, refs}, in file order."""
    entries = []
    cur, field, done = None, None, False

    def flush():
        if cur and cur["id"]:
            entries.append(cur)

    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.strip()
        if not line:
            flush()
            cur, field, done = None, None, False
            continue
        starts_entry = line.startswith(("#", "msgctxt ", "msgid "))
        if cur is None or (done and starts_entry):
            flush()
            cur, field, done = {"ctx": None, "id": None, "plural": None, "refs": []}, None, False
        if line.startswith("#:"):
            cur["refs"].append(line[2:].strip())
        elif line.startswith("#"):
            continue
        elif line.startswith("msgctxt "):
            cur["ctx"], field = parse_po_token(line[8:]), "ctx"
        elif line.startswith("msgid_plural "):
            cur["plural"], field = parse_po_token(line[13:]), "plural"
        elif line.startswith("msgid "):
            cur["id"], field = parse_po_token(line[6:]), "id"
        elif line.startswith("msgstr"):
            field, done = None, True
        elif line.startswith('"') and field:
            cur[field] += parse_po_token(line)
    flush()
    return entries


def nplurals(path):
    m = re.search(r"nplurals\s*=\s*(\d+)", read(path)[:4000])
    return int(m.group(1)) if m else 2


def entry_lines(e, plurals):
    out = ["#: %s" % r for r in e["refs"]]
    if e["ctx"] is not None:
        out.append('msgctxt "%s"' % po_escape(e["ctx"]))
    out.append('msgid "%s"' % po_escape(e["id"]))
    if e["plural"] is not None:
        out.append('msgid_plural "%s"' % po_escape(e["plural"]))
        out += ['msgstr[%d] ""' % i for i in range(plurals)]
    else:
        out.append('msgstr ""')
    return out


def append_entries(path, entries):
    body = open(path, encoding="utf-8", errors="replace", newline="").read()
    nl = "\r\n" if "\r\n" in body else "\n"
    plurals = nplurals(path)
    lines = [] if body.endswith(nl) else [""]
    lines += ["", "# === Added by i18n/extract_strings.py ==="]
    for e in entries:
        lines.append("")
        lines += entry_lines(e, plurals)
    with open(path, "a", encoding="utf-8", newline="") as f:
        f.write(nl.join(lines) + nl)


def key_of(e):
    return (e["ctx"], e["id"])


def main():
    dry = "--dry-run" in sys.argv
    verb = "new" if dry else "appended"
    keys = extract_keys()
    gtk = extract_gtk()
    print("win32/Qt normalized keys: %d, GTK msgids: %d" % (len(keys), len(gtk)))

    template = list({key_of(e): e for e in reversed(parse_entries(POT))}.values())[::-1]
    present = {key_of(e) for e in template}
    new = [{"ctx": None, "id": k, "plural": None, "refs": [keys[k]]}
           for k in sorted(keys, key=lambda s: s.lower()) if (None, k) not in present]
    present |= {key_of(e) for e in new}
    new += [{"ctx": c, "id": i, "plural": v["plural"], "refs": [v["ref"]]}
            for (c, i), v in sorted(gtk.items(), key=lambda kv: (kv[0][1].lower(), kv[0][0] or ""))
            if (c, i) not in present]
    print("  %-16s already=%-4d %s=%-4d" % ("snes9x.pot", len(template), verb, len(new)))
    if new and not dry:
        append_entries(POT, new)
    template += new

    for f in sorted(os.listdir(SCRIPT_DIR)):
        if not f.endswith(".po"):
            continue
        path = os.path.join(SCRIPT_DIR, f)
        have = {key_of(e) for e in parse_entries(path)}
        todo = [e for e in template if key_of(e) not in have]
        print("  %-16s already=%-4d %s=%-4d" % (f, len(template) - len(todo), verb, len(todo)))
        if todo and not dry:
            append_entries(path, todo)


if __name__ == "__main__":
    main()
