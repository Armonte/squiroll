"""
Import th155-system.csv into IDA.
CSV format: RVA,label_type,label
- ANALYSIS_LABEL with STRUCT annotations describe struct fields in .data.
"""
import re
import ida_name
import ida_bytes
import idc

CSV_PATH = r"C:\dev\aocf\repos\squiroll\tools\th155-system.csv"

named = 0
skipped = 0
errors = []

with open(CSV_PATH, "r", encoding="utf-8", errors="replace") as f:
    header = f.readline()  # skip header
    for line in f:
        line = line.strip()
        if not line:
            continue
        parts = line.split(",", 2)
        if len(parts) < 3:
            continue
        rva_str = parts[0].strip()
        label_type = parts[1].strip()
        label = parts[2].strip()
        if not rva_str or not label:
            continue
        # We skip structure annotations starting with "<STRUCT" — those are
        # informational metadata, not symbol names.
        if label.startswith("<"):
            skipped += 1
            continue
        try:
            rva = int(rva_str, 16)
        except ValueError:
            continue
        ea = rva

        if not ida_bytes.is_loaded(ea):
            continue

        current = ida_name.get_name(ea)
        if current == label:
            skipped += 1
            continue

        if ida_name.set_name(ea, label,
                             ida_name.SN_FORCE | ida_name.SN_NOWARN |
                             ida_name.SN_NOCHECK):
            named += 1
        else:
            errors.append(f"@ {rva:08X}: {label[:60]}")

print(f"Named: {named}, Skipped: {skipped}, Errors: {len(errors)}")
for e in errors[:15]:
    print(f"  {e}")
