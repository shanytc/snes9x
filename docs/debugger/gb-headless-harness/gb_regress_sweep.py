#!/usr/bin/env python3
# Run the gb_regress framebuffer-signature harness against every ROM zip in a
# directory and print one signature line per ROM. Diff the output of two runs
# (before vs after a core change) to find graphics/timing regressions:
#
#   python3 gb_regress_sweep.py win32/Roms 1200 > /tmp/baseline.txt   # before
#   ...make change, rebuild /tmp/gb_regress...
#   python3 gb_regress_sweep.py win32/Roms 1200 > /tmp/after.txt      # after
#   diff /tmp/baseline.txt /tmp/after.txt                             # changed ROMs
#
# Requires /tmp/gb_regress (built from gb_regress.cpp). Deterministic (no input).
import os, sys, glob, zipfile, subprocess, tempfile

REGRESS = "/tmp/gb_regress"
roms_dir = sys.argv[1] if len(sys.argv) > 1 else "win32/Roms"
frames   = sys.argv[2] if len(sys.argv) > 2 else "1200"

for zp in sorted(glob.glob(os.path.join(roms_dir, "*.zip"))):
    try:
        z = zipfile.ZipFile(zp)
        names = [n for n in z.namelist() if n.lower().endswith((".gb", ".gbc", ".sgb"))]
        if not names:
            continue
        data = z.read(names[0])
        ext = os.path.splitext(names[0])[1]
        with tempfile.NamedTemporaryFile(suffix=ext, delete=False) as tf:
            tf.write(data); rom_tmp = tf.name
        out = subprocess.run([REGRESS, rom_tmp, frames], capture_output=True, text=True, timeout=120)
        os.unlink(rom_tmp)
        line = out.stdout.strip()
        # Replace the temp basename with the zip's real name for stable diffs.
        if line:
            parts = line.split("\t", 1)
            print(os.path.basename(zp) + "\t" + (parts[1] if len(parts) > 1 else line))
        else:
            print(os.path.basename(zp) + "\tERROR\t" + out.stderr.strip()[:80])
    except Exception as e:
        print(os.path.basename(zp) + "\tEXC\t" + str(e)[:80])
    sys.stdout.flush()
