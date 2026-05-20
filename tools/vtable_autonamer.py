"""Vtable autonamer for th155.exe.

For each `??_7ClassName@@6B@` symbol (MSVC vtable), reads function
pointers sequentially until we hit a non-function-pointer or the next
RTTI artifact. Names each entry `<DemangledClass>__vftable_<index>` if
it's currently sub_*.

Conservative — leaves anything already hand-named or autonamed by the
Sqrat pass alone.
"""

import idautils, idaapi, idc, ida_funcs, ida_name, ida_bytes
import re

# Standard MSVC mangling: ??_7<Class>@@6B@ = const ClassName::`vftable'
# Some templates: ??_7?$Class@VManbow::Actor2D@@@@6B@ etc.
VFTABLE_PATTERN = re.compile(r"^\?\?_7(.+?)@@6B[A-Za-z0-9_]*@$")

MAX_SLOTS = 80  # Sanity cap — even AnimationControllerBase only has ~45.


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9_]+", "_", s)
    # IDA tends to choke on names longer than 250-ish chars; cap.
    return s[:200]


def is_func_addr(ea):
    f = ida_funcs.get_func(ea)
    return f is not None and f.start_ea == ea


def demangle_class(mangled):
    """Try IDA's demangler first; fall back to regex extraction."""
    out = idc.demangle_name(mangled, idc.get_inf_attr(idc.INF_LONG_DEMNAMES))
    if out:
        # e.g. "const Manbow::Actor2D::`vftable'" → "Manbow::Actor2D"
        m = re.match(r"^const (.+)::`vftable'", out)
        if m:
            return m.group(1)
    # Fallback: parse the mangled symbol.
    m = VFTABLE_PATTERN.match(mangled)
    if m:
        return m.group(1)
    return mangled


# Find every vtable symbol.
vtables = []
for ea, name in idautils.Names():
    if "?_7" in name and "@@6B" in name:
        cls = demangle_class(name)
        vtables.append((ea, cls))

print(f"Found {len(vtables)} vtables")

named_count = 0
slot_count = 0
total_examined = 0

for vt_ea, cls in vtables:
    cls_clean = safe_name(cls)
    for slot in range(MAX_SLOTS):
        slot_ea = vt_ea + slot * 4
        # Stop at the next RTTI symbol or labelled boundary.
        n = ida_name.get_name(slot_ea)
        if slot > 0 and n:
            # Hit the next labelled item — vtable ends here.
            break
        # Read the pointer.
        ptr = idc.get_wide_dword(slot_ea)
        if not is_func_addr(ptr):
            break
        total_examined += 1
        slot_count += 1
        fn_name = ida_name.get_name(ptr) or ""
        if not fn_name.startswith("sub_"):
            continue
        new = f"{cls_clean}__vftable_{slot}"
        if ida_name.set_name(ptr, new, ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
            named_count += 1

print(f"Walked {slot_count} vtable slots ({total_examined} unique function pointers); renamed {named_count}")
print("Done.")
