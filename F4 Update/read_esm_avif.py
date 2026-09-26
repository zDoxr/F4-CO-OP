"""
Parse Fallout4.esm to find ActorValueInfo (AVIF) records and their form IDs.
Run directly: python read_esm_avif.py
"""
import struct, sys, os

ESM_PATH = r"D:\SteamLibrary\steamapps\common\Fallout 4\Data\Fallout4.esm"
OUT_FILE = r"C:\Users\hotto\source\repos\DoxCoSync\F4 Update\esm_avif_result.txt"

SPECIAL_NAMES = {
    b'Strength', b'Perception', b'Endurance', b'Charisma',
    b'Intelligence', b'Agility', b'Luck',
    b'Health', b'ActionPoints', b'Action Points',
    b'Rads', b'RadiationExposure', b'Fatigue', b'SpeedMult',
    b'Speed Mult', b'CarryWeight', b'Carry Weight'
}

def read_record_header(f):
    """Read a 24-byte TESV record header."""
    data = f.read(24)
    if len(data) < 24:
        return None
    sig = data[0:4]
    data_size = struct.unpack_from('<I', data, 4)[0]
    flags = struct.unpack_from('<I', data, 8)[0]
    form_id = struct.unpack_from('<I', data, 12)[0]
    return sig, data_size, flags, form_id

def read_group_header(f):
    """Read a 24-byte TESV group header."""
    data = f.read(24)
    if len(data) < 24:
        return None
    sig = data[0:4]  # 'GRUP'
    group_size = struct.unpack_from('<I', data, 4)[0]
    label = data[8:12]
    group_type = struct.unpack_from('<i', data, 12)[0]
    return sig, group_size, label, group_type

results = []

with open(ESM_PATH, 'rb') as f, open(OUT_FILE, 'w', encoding='utf-8') as out:
    out.write("Fallout4.esm AVIF (ActorValueInfo) Record Scanner\n")
    out.write("=" * 60 + "\n\n")

    # Parse the ESM - scan for all AVIF records
    file_size = os.path.getsize(ESM_PATH)
    out.write(f"File size: {file_size:,} bytes\n\n")

    # Skip the TES4 header record first
    header = read_record_header(f)
    if header:
        sig, data_size, flags, form_id = header
        out.write(f"Header: sig={sig}, size={data_size}, formID=0x{form_id:08X}\n")
        f.seek(data_size, 1)  # Skip header data

    # Now scan for AVIF records
    avif_found = 0
    scanned = 0

    # We'll do a sequential scan looking for 'AVIF' signature
    # More robust: just search for AVIF bytes
    f.seek(0)
    data = f.read()

with open(OUT_FILE, 'a', encoding='utf-8') as out:
    out.write(f"\nLoaded {len(data):,} bytes into memory\n\n")
    out.write("Scanning for AVIF records...\n\n")

    pos = 0
    avif_records = []
    AVIF_SIG = b'AVIF'

    while pos < len(data) - 24:
        idx = data.find(AVIF_SIG, pos)
        if idx == -1:
            break

        # Check if this looks like a valid record header
        # (not inside a GRUP label — check context)
        # Record header: SIG(4) + DataSize(4) + Flags(4) + FormID(4) + ...
        if idx + 24 <= len(data):
            try:
                rec_data_size = struct.unpack_from('<I', data, idx + 4)[0]
                rec_flags = struct.unpack_from('<I', data, idx + 8)[0]
                rec_form_id = struct.unpack_from('<I', data, idx + 12)[0]
                rec_timestamp = struct.unpack_from('<H', data, idx + 16)[0]

                # Sanity check: data_size should be reasonable (<500KB), formID > 0
                if 0 < rec_data_size < 500000 and rec_form_id > 0 and rec_form_id < 0x01000000:
                    rec_start = idx + 24
                    rec_end = rec_start + rec_data_size

                    if rec_end <= len(data):
                        # Parse subrecords to find FULL (full name) or EDID (editor ID)
                        full_name = b''
                        edid = b''
                        sub_pos = rec_start
                        while sub_pos < rec_end - 6:
                            sub_sig = data[sub_pos:sub_pos+4]
                            sub_size = struct.unpack_from('<H', data, sub_pos+4)[0]
                            sub_data = data[sub_pos+6:sub_pos+6+sub_size]
                            if sub_sig == b'EDID':
                                edid = sub_data.rstrip(b'\x00')
                            elif sub_sig == b'FULL':
                                full_name = sub_data.rstrip(b'\x00')
                            sub_pos += 6 + sub_size

                        avif_records.append((rec_form_id, edid, full_name))
                        pos = rec_end
                        continue
            except Exception:
                pass
        pos = idx + 1

    avif_records.sort(key=lambda x: x[0])

    out.write(f"Found {len(avif_records)} AVIF records\n\n")
    out.write(f"{'FormID':<12} {'EditorID':<40} {'FullName'}\n")
    out.write("-" * 80 + "\n")

    for form_id, edid, full_name in avif_records:
        edid_str = edid.decode('latin-1', errors='replace')
        full_str = full_name.decode('latin-1', errors='replace') if full_name else ''
        out.write(f"0x{form_id:08X}   {edid_str:<40} {full_str}\n")

        # Flag SPECIAL stats
        if any(name.lower() in edid.lower() or name.lower() in full_name.lower()
               for name in [b'strength', b'perception', b'endurance', b'charisma',
                             b'intelligence', b'agility', b'luck', b'health',
                             b'actionpoints', b'carryweight', b'speedmult', b'rads']):
            out.write(f"  *** SPECIAL/CORE STAT ***\n")

    out.write(f"\nDone. Total AVIF records: {len(avif_records)}\n")

print(f"Done. Results in {OUT_FILE}")
