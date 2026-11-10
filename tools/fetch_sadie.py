#!/usr/bin/env python3
"""Downloads SADIE II head (HRTF) sets into data/hrtf/sadie2/ (standard library only).

    python tools/fetch_sadie.py            # D1 (KU100) + D2 (KEMAR): what the installer ships
    python tools/fetch_sadie.py --all      # + the 18 human subjects H3..H20 (~240 MB total)

Source: SADIE II database, University of York, Apache License 2.0
        https://zenodo.org/records/12092466
Each subject's <id>_HRIR_SOFA.zip is downloaded and only its 48 kHz SOFA file is kept,
saved as data/hrtf/sadie2/<id>.sofa (the name TacAudio looks for).
"""

import argparse
import io
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "hrtf", "sadie2")
BASE = "https://zenodo.org/api/records/12092466/files"


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "TacAudio-build"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="also download the human subjects H3..H20")
    args = ap.parse_args()

    subjects = ["D1", "D2"] + ([f"H{i}" for i in range(3, 21)] if args.all else [])
    os.makedirs(OUT, exist_ok=True)
    for s in subjects:
        dst = os.path.join(OUT, f"{s}.sofa")
        if os.path.exists(dst):
            print(f"{s}: already present")
            continue
        print(f"{s}: downloading...", flush=True)
        data = fetch(f"{BASE}/{s}_HRIR_SOFA.zip/content")
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.endswith("_48K_24bit_256tap_FIR_SOFA.sofa")]
            if len(names) != 1:
                sys.exit(f"{s}: unexpected archive layout {z.namelist()}")
            with z.open(names[0]) as src, open(dst, "wb") as out:
                out.write(src.read())
        print(f"{s}: saved {dst}")
    lic = os.path.join(OUT, "LICENSE.txt")
    if not os.path.exists(lic):
        with open(lic, "wb") as f:
            f.write(fetch(f"{BASE}/LICENSE.txt/content"))
    print("done")


if __name__ == "__main__":
    main()
