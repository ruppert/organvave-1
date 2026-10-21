#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# OrganVave-1's site with the installer: one self-contained page (site_organ.html, with the package embedded) and the
# package itself for the download. The installer's JS is fm1pkg.js and fm1ota.js (the FMini-1 site's, GPL-3.0-only).
#   python3 web/make_site.py build/felucca.fwsc OUTDIR VERSION
# writes OUTDIR/index.html and OUTDIR/organvave-1-VERSION.fwsc. VERSION must be ORGANVAVE_VERSION in firmware/src/core.h.
# Serve it over https or http://localhost: Web MIDI with SysEx needs a secure context (Chrome or Edge).
import base64
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def main(pkg, outdir, version):
    core = (ROOT / "firmware" / "src" / "core.h").read_text(encoding="utf-8")
    if f'#define ORGANVAVE_VERSION "v{version}"' not in core:
        raise SystemExit(f"VERSION {version} is not ORGANVAVE_VERSION in firmware/src/core.h")
    raw = Path(pkg).read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_8\d\d", product) or b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: not a Felucca-loader package (identity {product!r})")
    html = (HERE / "site_organ.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((HERE / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js"))
    url = "data:application/octet-stream;base64," + base64.b64encode(raw).decode()
    file = f"organvave-1-{version}.fwsc"
    meta = json.dumps({"version": version, "product": product, "pkg": url, "file": file})
    for mark in ("/*LIB*/", "/*META*/{}"):
        if html.count(mark) != 1:
            raise SystemExit(f"site_organ.html must contain {mark} once")
    html = html.replace("/*LIB*/", lib).replace("/*META*/{}", meta + ";").replace("/*VERSION*/", version)
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(html, encoding="utf-8")
    shutil.copyfile(pkg, out / file)
    print(f"{out}/index.html: {len(html)} B, {product}, package {len(raw)} B embedded; {file} beside it")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
