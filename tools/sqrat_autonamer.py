"""Sqrat binding autonamer for th155.exe.

th155.exe statically links Sqrat 0.8.3. The per-class registration code
("SqratSetup" functions) constructs a Sqrat::Class<T>, calls
Sqrat::Object::BindFunc once per method, then Sqrat::TableBase::Bind once to
publish the class under its script name. A single setup function may register
SEVERAL classes back-to-back, so each BindFunc call is attributed to the
NEAREST FOLLOWING TableBase::Bind in the same function.

Anchor functions (RVAs, imagebase 0):
  SQRAT_TABLEBASE_BIND  = 0xB9E0   Sqrat::TableBase::Bind(this, name, table)
  SQRAT_OBJECT_BINDFUNC = 0xB5B0   Sqrat::Object::BindFunc(this, name, &Src,
                                                           Size, thunk, isStatic)

BindFunc call-site shape (args pushed right-to-left):
    push  <isStatic>
    push  offset <dispatch thunk>
    push  <Size>
    lea   eax, [ebp+Src] ; push eax            ; &Src
    push  offset <name str>
    mov   [ebp+Src], offset <C++ method>       ; the bound function
    call  Sqrat__Object__BindFunc

The bound C++ function is renamed <Class>__<method>.
"""

import idautils, idaapi, idc, ida_funcs, ida_name
import re

SQRAT_TABLEBASE_BIND  = 0xB9E0
SQRAT_OBJECT_BINDFUNC = 0xB5B0
SCAN_BYTES = 0x80

# An earlier buggy pass mis-attributed many bound functions (grabbed the wrong
# push, producing off-by-one method names and garbage class prefixes). When
# OVERWRITE_WRONG is on, Phase 2 also corrects any existing <X>__<Y> name that
# disagrees with the freshly-recovered <Class>__<method>.
OVERWRITE_WRONG = True


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
    """mov [reg+disp], imm  ->  imm  (the `Src = offset Func`)."""
    if idc.print_insn_mnem(ea).lower() != "mov":
        return None
    if idc.get_operand_type(ea, 0) != idc.o_displ:
        return None
    if idc.get_operand_type(ea, 1) != idc.o_imm:
        return None
    return idc.get_operand_value(ea, 1)


def is_func_start(ea):
    f = ida_funcs.get_func(ea)
    return f is not None and f.start_ea == ea


def name_string_back(call_ea):
    """First push of a C identifier-shaped string before call_ea."""
    for ea in walk_back(call_ea, SCAN_BYTES):
        imm = push_imm_at(ea)
        if imm is None:
            continue
        s = read_cstring(imm)
        if s and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", s):
            return s
    return None


# --------------------------------------------------------------------------
# Phase 0: collect every TableBase::Bind (class publish) keyed by call addr.
# --------------------------------------------------------------------------
bind_calls = []                       # (call_ea, setup_ea, class_name)
setup_first_class = {}                # setup_ea -> class name of FIRST bind
for call_ea, setup_ea in call_sites(SQRAT_TABLEBASE_BIND):
    cls = name_string_back(call_ea)
    if not cls:
        continue
    bind_calls.append((call_ea, setup_ea, cls))

bind_calls.sort(key=lambda t: t[0])
for call_ea, setup_ea, cls in bind_calls:
    if setup_ea not in setup_first_class:
        setup_first_class[setup_ea] = cls

print(f"Phase 0: {len(bind_calls)} TableBase::Bind sites across "
      f"{len(setup_first_class)} setup functions")


def class_for_bindfunc(call_ea, setup_ea):
    """Class of the nearest TableBase::Bind that FOLLOWS call_ea in setup."""
    best = None
    for bc_ea, bc_setup, cls in bind_calls:
        if bc_setup != setup_ea:
            continue
        if bc_ea >= call_ea and (best is None or bc_ea < best[0]):
            best = (bc_ea, cls)
    if best:
        return best[1]
    # fall back: last Bind in the setup (BindFunc after final Bind is rare)
    same = [c for (e, s, c) in bind_calls if s == setup_ea]
    return same[-1] if same else None


# --------------------------------------------------------------------------
# Phase 1: rename SqratSetup functions still anonymous.
# --------------------------------------------------------------------------
renamed_setups = 0
for setup_ea, cls in setup_first_class.items():
    cur = ida_name.get_name(setup_ea) or ""
    desired = f"{safe_name(cls)}__SqratSetup"
    if cur == desired or not cur.startswith("sub_"):
        continue
    if ida_name.set_name(setup_ea, desired,
                         ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
        renamed_setups += 1
print(f"Phase 1: renamed {renamed_setups} SqratSetup functions")


# --------------------------------------------------------------------------
# Phase 2: per BindFunc call site, name the bound C++ function.
# --------------------------------------------------------------------------
renamed_methods = 0
examined = 0
skipped_named = 0
no_match = 0
collisions = {}
for call_ea, setup_ea in call_sites(SQRAT_OBJECT_BINDFUNC):
    examined += 1
    cls = class_for_bindfunc(call_ea, setup_ea)
    if not cls:
        continue
    cls = safe_name(cls)

    method_name = None
    func_addr = None
    for ea in walk_back(call_ea, SCAN_BYTES):
        if method_name is None:
            imm = push_imm_at(ea)
            if imm is not None:
                s = read_cstring(imm)
                if s and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", s):
                    method_name = s
        if func_addr is None:
            imm = mov_stack_imm_at(ea)
            if imm is not None and is_func_start(imm):
                func_addr = imm
        if method_name and func_addr:
            break

    if method_name is None or func_addr is None:
        no_match += 1
        continue

    cur = ida_name.get_name(func_addr) or ""
    base = f"{cls}__{safe_name(method_name)}"
    n = collisions.get(base, 0)
    collisions[base] = n + 1
    desired = base if n == 0 else f"{base}_{n}"

    if cur == desired:
        continue
    if not cur.startswith("sub_"):
        # Already named. Overwrite only when the existing name looks like a
        # prior (buggy) autoname, i.e. exactly <ClassPart>__<methodPart> with a
        # single "__" separator. Protect:
        #   - IDA mangled / library names ("?..."), which are more precise
        #   - explicit C++-namespace names like Manbow__Class__Method (2x "__")
        #   - core Sqrat/runtime helpers
        ok_prefixes = ("Sqrat__", "sq_", "std", "operator",
                       "j_", "?", "init_", "_")
        if not OVERWRITE_WRONG or cur.startswith(ok_prefixes):
            skipped_named += 1
            continue
        # require exactly one "__" (a plain 2-part autoname); names with two
        # "__" carry an explicit namespace and are left intact.
        if cur.count("__") != 1 or not re.match(
                r"^[A-Za-z][A-Za-z0-9_]*__[A-Za-z][A-Za-z0-9_]*$", cur):
            skipped_named += 1
            continue
    if ida_name.set_name(func_addr, desired,
                         ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
        renamed_methods += 1

print(f"Phase 2: examined {examined} BindFunc calls, "
      f"renamed {renamed_methods} bound functions "
      f"({skipped_named} already had non-sub_ names, "
      f"{no_match} unresolved)")
print("Done.")
