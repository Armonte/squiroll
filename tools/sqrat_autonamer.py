"""Sqrat binding autonamer for th155.exe.

Recovery + autonamer: fixes earlier passes that grabbed the wrong push
(2nd back instead of 1st — yielded method names where ClassName should
be) and continues with the correct logic.
"""

import idautils, idaapi, idc, ida_funcs, ida_name
import re

SQRAT_TABLEBASE_BIND  = 0x40B9E0
SQRAT_OBJECT_BINDFUNC = 0x40B5B0
SCAN_BYTES = 0xC0


def read_cstring(ea):
    if not ea or ea == idaapi.BADADDR:
        return None
    s = idc.get_strlit_contents(ea, -1, idc.STRTYPE_C)
    return s.decode("utf-8", errors="replace") if s else None


def safe_name(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s)


def call_sites(target_ea):
    for xr in idautils.XrefsTo(target_ea, 0):
        if xr.type not in (idaapi.fl_CN, idaapi.fl_CF):
            continue
        f = ida_funcs.get_func(xr.frm)
        if f is None:
            continue
        yield xr.frm, f.start_ea


def walk_back(call_ea, span):
    end = call_ea - span
    ea = idc.prev_head(call_ea, end)
    while ea != idaapi.BADADDR and ea > end:
        yield ea
        ea = idc.prev_head(ea, end)


def push_imm_at(ea):
    if idc.print_insn_mnem(ea).lower() != "push":
        return None
    if idc.get_operand_type(ea, 0) != idc.o_imm:
        return None
    return idc.get_operand_value(ea, 0)


def mov_stack_imm_at(ea):
    if idc.print_insn_mnem(ea).lower() != "mov":
        return None
    if idc.get_operand_type(ea, 0) != idc.o_displ:
        return None
    if idc.get_operand_type(ea, 1) != idc.o_imm:
        return None
    return idc.get_operand_value(ea, 0), idc.get_operand_value(ea, 1)


# Phase 0: compute correct ClassName per SqratSetup (CLOSEST push only).
setup_class = {}
for call_ea, setup_ea in call_sites(SQRAT_TABLEBASE_BIND):
    for ea in walk_back(call_ea, SCAN_BYTES):
        imm = push_imm_at(ea)
        if imm is None:
            continue
        s = read_cstring(imm)
        if s and re.match(r"^[A-Za-z_][A-Za-z0-9_]+$", s):
            setup_class[setup_ea] = s
        break

print(f"Phase 0: {len(setup_class)} SqratSetup classes identified")


# Phase 1: build map of bad earlier renames -> correct prefix.
bad_to_good = {}
for setup_ea, good in setup_class.items():
    cur = ida_name.get_name(setup_ea) or ""
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)__SqratSetup$", cur)
    if m and m.group(1) != safe_name(good):
        bad_to_good[m.group(1)] = safe_name(good)

print(f"Phase 1: {len(bad_to_good)} bad ClassName prefixes to remap:")
for bad, good in sorted(bad_to_good.items()):
    print(f"  {bad}__ -> {good}__")

# Fix every function with a bad prefix.
fixed = 0
for fn_ea in idautils.Functions():
    cur = ida_name.get_name(fn_ea)
    if not cur:
        continue
    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)__([A-Za-z_][A-Za-z0-9_]*)$", cur)
    if not m:
        continue
    pfx, suffix = m.group(1), m.group(2)
    if pfx in bad_to_good:
        new = f"{bad_to_good[pfx]}__{suffix}"
        if ida_name.set_name(fn_ea, new, ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
            fixed += 1
print(f"Phase 1: fixed {fixed} functions with corrected ClassName prefix")


# Phase 2: rename SqratSetup functions still as sub_*.
renamed_setups = 0
for setup_ea, cls in setup_class.items():
    cur = ida_name.get_name(setup_ea) or ""
    desired = f"{safe_name(cls)}__SqratSetup"
    if cur == desired:
        continue
    if cur.startswith("sub_") or re.match(r"^[A-Za-z_][A-Za-z0-9_]*__SqratSetup$", cur):
        if ida_name.set_name(setup_ea, desired, ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
            renamed_setups += 1
print(f"Phase 2: renamed {renamed_setups} SqratSetup functions")


# Phase 3: per SqratSetup, autoname BindFunc targets.
bindfunc_by_setup = {}
for call_ea, setup_ea in call_sites(SQRAT_OBJECT_BINDFUNC):
    if setup_ea not in setup_class:
        continue
    bindfunc_by_setup.setdefault(setup_ea, []).append(call_ea)

renamed_methods = 0
examined = 0
for setup_ea, calls in bindfunc_by_setup.items():
    cls = safe_name(setup_class[setup_ea])
    for call_ea in calls:
        examined += 1
        method_name = None
        src_off = None
        func_addr = None

        for ea in walk_back(call_ea, SCAN_BYTES):
            mnem = idc.print_insn_mnem(ea).lower()

            if method_name is None:
                imm = push_imm_at(ea)
                if imm is None:
                    continue
                s = read_cstring(imm)
                if s and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", s):
                    method_name = s
                continue

            if src_off is None and mnem == "lea":
                if idc.get_operand_type(ea, 1) == idc.o_displ:
                    src_off = idc.get_operand_value(ea, 1)
                continue

            if src_off is not None and mnem == "mov":
                slot = mov_stack_imm_at(ea)
                if slot is not None:
                    disp, imm = slot
                    if disp == src_off:
                        f = ida_funcs.get_func(imm)
                        if f and f.start_ea == imm:
                            func_addr = imm
                        break

        if method_name is None or func_addr is None:
            continue

        cur = ida_name.get_name(func_addr) or ""
        desired = f"{cls}__{safe_name(method_name)}"
        if cur == desired:
            continue
        if not cur.startswith("sub_"):
            continue
        if ida_name.set_name(func_addr, desired,
                             ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
            renamed_methods += 1

print(f"Phase 3: examined {examined} BindFunc calls, renamed {renamed_methods} target functions")
print("Done.")
