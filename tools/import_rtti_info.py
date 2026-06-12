"""
Parse rtti_info.txt and apply vtable / class names to IDA.

Format:
    Rx{hex_rva}\tclass {mangled_or_demangled_name}
    \tVirtual Function Table ({count}):
    \t\t{slot}\t{idx}\t\t{ref}\t{symbol}
    ...

We just take the first column (class name + vtable address) and apply it
to the IDA address. The slot rows are informational; if we wanted to,
we'd patch vtable types too.
"""
import re
import ida_name
import ida_bytes
import idc

RTTI_PATH = r"C:\dev\aocf\repos\squiroll\tools\rtti_info.txt"

vtables_named = 0
errors = []

with open(RTTI_PATH, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        # Match the class definition line at the start of a vtable block.
        # "    Rx38b788\tclass ??_7?..."
        m = re.match(r"^\s*Rx([0-9a-fA-F]+)\s+class\s+(.+?)\s*$", line)
        if not m:
            continue
        rva = int(m.group(1), 16)
        mangled = m.group(2).strip()

        # The mangled names start with ??_7 for vtables. Some have leading
        # ?? and need to be demangled. IDA can demangle directly.
        # For now we'll just set the symbol name as-is; IDA's name parser
        # demangles ??_7... vtables to "<Class>::`vftable'".
        ea = rva  # imagebase 0

        if not ida_bytes.is_loaded(ea):
            errors.append(f"not loaded @ {rva:08X}: {mangled[:60]}")
            continue

        # Set the mangled name; IDA's demangler will display it nicely.
        if ida_name.set_name(ea, mangled,
                             ida_name.SN_FORCE | ida_name.SN_NOWARN |
                             ida_name.SN_NOCHECK):
            vtables_named += 1
        else:
            errors.append(f"rename failed @ {rva:08X}: {mangled[:60]}")

print(f"VTables named: {vtables_named}, Errors: {len(errors)}")
for e in errors[:15]:
    print(f"  {e}")
