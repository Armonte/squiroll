"""
Bulk import th155.csv labels into IDA.
CSV format: RVA,label,comment   (RVA = hex without 0x prefix)
"""
import idaapi
import ida_name
import ida_funcs
import idc

CSV_PATH = r"C:\dev\aocf\repos\squiroll\tools\th155.csv"

renamed = 0
skipped = 0
errors = []

with open(CSV_PATH, "r") as f:
    header = f.readline()  # skip header
    for line in f:
        line = line.strip()
        if not line:
            continue
        parts = line.split(",", 2)
        if len(parts) < 2:
            continue
        rva_str = parts[0].strip()
        label = parts[1].strip()
        if not rva_str or not label:
            continue
        try:
            rva = int(rva_str, 16)
        except ValueError:
            errors.append(f"bad rva: {rva_str}")
            continue

        ea = rva  # imagebase is 0 in our IDB, so RVA == EA

        # Skip if already named to this label
        current = ida_name.get_name(ea)
        if current == label:
            skipped += 1
            continue

        # Only set if address has a defined function / instruction. Otherwise it'd
        # create a label at an arbitrary location.
        flags = idc.get_full_flags(ea)
        if flags == 0:
            errors.append(f"no flags @ {rva:08X} for {label}")
            continue

        # Set the name. SN_FORCE = override existing name, SN_NOWARN = quiet.
        if ida_name.set_name(ea, label, ida_name.SN_FORCE | ida_name.SN_NOWARN):
            renamed += 1
        else:
            errors.append(f"rename failed @ {rva:08X} for {label}")

print(f"Renamed: {renamed}, Skipped: {skipped}, Errors: {len(errors)}")
for e in errors[:20]:
    print(f"  {e}")
