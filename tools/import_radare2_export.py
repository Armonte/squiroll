"""
Import names from a radare2 / rizin function export of th155.exe.

Format per line:
    0x{vaddr_with_400000_base}  {blocks} {size} [-> {ret_size}]  {name}

Mapping rules (conservative — we only OVERWRITE names that start with `sub_`,
`j_sub_`, `__sub_`, or `loc_`; anything we already curated stays):

- name starts with `fcn.`           — radare2 placeholder. SKIP.
- name starts with `sub.`           — radare2 descriptor. KEEP for entries with
                                      KERNEL32/USER32/etc-style descriptors;
                                      otherwise skip (they're just placeholders).
- name starts with `method.X.virtual_N` — convert to `X__virtual_N`
                                          (sanitised for IDA identifier rules)
- name starts with `flirt.`         — FLIRT-matched library symbol; convert
                                      to `flirt_{rest}`
- plain `Class::method` style       — use as-is (these are manually annotated
                                      and the highest-quality)

Address translation: radare2 used a 0x00400000 image base. Our IDB has base 0.
So `idb_ea = radare2_va - 0x400000`.
"""
import re
import ida_name
import ida_bytes

PATH = r"C:\dev\aocf\repos\squiroll\tools\th155_db_export.txt"
RADARE2_BASE = 0x00400000

# Identifier-safe replacement for radare2's punctuation.
_SANITIZE = str.maketrans({
    ':': '_', '.': '_', '<': '_', '>': '_', ',': '_',
    ' ': '_', '*': 'P', '&': 'R', '(': '_', ')': '_',
    '[': '_', ']': '_', '?': '_', '!': '_', '$': '_',
    '+': 'p', '-': 'm', '/': '_', '\\': '_', '=': 'eq',
    '"': '_', '\'': '_', ';': '_', '|': '_',
})


def sanitize(name: str) -> str:
    """Collapse any sequence of underscores to a single one for readability."""
    s = name.translate(_SANITIZE)
    s = re.sub(r'_+', '_', s).strip('_')
    return s


renamed = 0
skipped_fcn = 0
skipped_sub = 0
skipped_preserved = 0
skipped_not_loaded = 0
errors = []

LINE_RE = re.compile(
    r'^0x([0-9a-fA-F]+)\s+\d+\s+\d+\s*(?:->\s*\d+)?\s+(\S.*?)\s*$')

with open(PATH, 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        m = LINE_RE.match(line)
        if not m:
            continue
        va = int(m.group(1), 16)
        name = m.group(2).strip()

        ea = va - RADARE2_BASE
        if ea < 0:
            continue

        if not ida_bytes.is_loaded(ea):
            skipped_not_loaded += 1
            continue

        # ---- name classification ----
        if name.startswith('fcn.'):
            skipped_fcn += 1
            continue

        if name.startswith('sub.'):
            # Most sub.X entries are placeholders — but the descriptor (after
            # the prefix) often encodes the import call (KERNEL32.dll_CreateEventA)
            # or a virtual-base description. Keep those.
            desc = name[len('sub.'):]
            # Drop the trailing hex addr radare2 appends.
            desc = re.sub(r'_[0-9a-fA-F]+$', '', desc)
            if not desc or desc.startswith('Unknown') or len(desc) < 4:
                skipped_sub += 1
                continue
            ident = 'sub_' + sanitize(desc)
        elif name.startswith('method.'):
            # method.Class.virtual_N — split on the last `.virtual_`.
            rest = name[len('method.'):]
            mm = re.match(r'^(.*)\.virtual_(\d+)$', rest)
            if not mm:
                # Different shape — just sanitise.
                ident = sanitize(rest)
            else:
                cls = sanitize(mm.group(1))
                idx = mm.group(2)
                ident = f'{cls}__virtual_{idx}'
        elif name.startswith('flirt.'):
            ident = 'flirt_' + sanitize(name[len('flirt.'):])
        elif name.startswith('loc.'):
            ident = 'loc_r_' + sanitize(name[len('loc.'):])
        else:
            # Pure manual name — sanitise but keep as much as possible.
            ident = sanitize(name)

        if not ident:
            continue

        current = ida_name.get_name(ea)
        if current == ident:
            continue

        # CONSERVATIVE: never overwrite a curated name.
        if current and not (current.startswith('sub_') or
                            current.startswith('j_sub_') or
                            current.startswith('__sub_') or
                            current.startswith('loc_') or
                            current.startswith('nullsub_') or
                            current.startswith('unknown_libname_')):
            skipped_preserved += 1
            continue

        if ida_name.set_name(ea, ident,
                             ida_name.SN_FORCE | ida_name.SN_NOWARN |
                             ida_name.SN_NOCHECK):
            renamed += 1
        else:
            errors.append(f'@ {ea:08X}: {ident[:70]}')

print(f'Renamed: {renamed}')
print(f'Skipped (fcn.):       {skipped_fcn}')
print(f'Skipped (sub. junk):  {skipped_sub}')
print(f'Skipped (preserved):  {skipped_preserved}')
print(f'Skipped (not loaded): {skipped_not_loaded}')
print(f'Errors: {len(errors)}')
for e in errors[:10]:
    print(f'  {e}')
