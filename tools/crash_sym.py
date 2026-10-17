#!/usr/bin/env python3
"""crash_sym.py ELF ADDR... : the function (and offset) each crash address falls in (objdump -t of the build that crashed)"""
import os, subprocess, sys
od = os.path.expanduser("~/.jieli/toolchain/common/bin/objdump")
syms = []
for l in subprocess.run([od, "-t", sys.argv[1]], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 5 and p[2] in ("F", "l", "g") and p[-2 if p[2] == "F" else -2]:
        try: a, sz = int(p[0], 16), int(p[-2], 16)
        except ValueError: continue
        if ".text" in l or "_text" in l: syms.append((a, sz, p[-1]))
syms.sort()
for s in sys.argv[2:]:
    a = int(s, 16); best = None
    for b, sz, n in syms:
        if b <= a and (best is None or b >= best[0]): best = (b, sz, n)
    print(f"{a:08X}  " + (f"{best[2]}+0x{a - best[0]:X} (size 0x{best[1]:X})" if best else "?"))
