"""
Parse rtti_info.txt vtable slot rows and name the target functions.

Format:
    Rx{vt_rva}\tclass {mangled_name}
    \tVirtual Function Table ({count}):
    \t\t{slot}\t{idx}\t\t{ref_rva}\t{symbol}

We use the symbol column (when not __purecall or empty) to name the target
function at ref_rva. Demangled by IDA via the SN_FORCE flag on a mangled
symbol it recognizes.
"""
import re
import ida_name
import ida_bytes

RTTI_PATH = r"C:\dev\aocf\repos\squiroll\tools\rtti_info.txt"

named_targets = 0
skipped_purecall = 0
errors = []

# Track the most-recent vtable class so we can derive a nice slot name.
current_vt_class = None

# Strip "??_7" prefix (vtable mangle marker) and "@6B@" suffix to get the
# "raw" class mangle, which we then prefix with "?" + index for method naming.
# Simpler: just use the slot's already-mangled symbol field directly.

with open(RTTI_PATH, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        # Class header line — note for context (not strictly needed).
        m_class = re.match(r"^\s*Rx([0-9a-fA-F]+)\s+class\s+(.+?)\s*$", line)
        if m_class:
            current_vt_class = m_class.group(2).strip()
            continue
        # Slot row — 4+ tab-separated fields after leading whitespace.
        m_slot = re.match(
            r"^\s*([0-9a-fA-F]+)\s+(\d+)\s+Rx([0-9a-fA-F]+)\s+(.+?)\s*$",
            line)
        if not m_slot:
            continue
        _slot_off = int(m_slot.group(1), 16)
        _slot_idx = int(m_slot.group(2))
        target_rva = int(m_slot.group(3), 16)
        symbol = m_slot.group(4).strip()

        # __purecall is the abstract-method placeholder — every unset slot
        # uses it. Don't rename a single shared function for all of them.
        if symbol == "__purecall" or not symbol:
            skipped_purecall += 1
            continue

        ea = target_rva
        if not ida_bytes.is_loaded(ea):
            continue

        current = ida_name.get_name(ea)
        if current == symbol:
            continue

        # CONSERVATIVE: only overwrite raw sub_XXXXX placeholders. Anything
        # we (or earlier imports) already named is more informative than a
        # generic mangled vtable slot symbol — keep it.
        # j_sub_ thunks are also fair game (they're just unresolved trampolines).
        if current and not (current.startswith("sub_") or
                            current.startswith("j_sub_") or
                            current.startswith("__sub_") or
                            current.startswith("loc_")):
            continue

        if ida_name.set_name(ea, symbol,
                             ida_name.SN_FORCE | ida_name.SN_NOWARN |
                             ida_name.SN_NOCHECK):
            named_targets += 1
        else:
            errors.append(f"@ {target_rva:08X}: {symbol[:60]}")

print(f"Named targets: {named_targets}, Skipped purecall: {skipped_purecall}, "
      f"Errors: {len(errors)}")
for e in errors[:15]:
    print(f"  {e}")
