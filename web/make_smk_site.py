#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
# OrganVave-1's installer page for the M-Vave SMK-37 (the SMK-37 port, SMK37.md): site_smk.html with the installer's JS
# (fm1pkg.js, fm1ota.js) and the three packages beside it.
#   python3 web/make_smk_site.py OUTDIR VERSION PRO.fwsc ELITE.fwsc MKE.fwsc
# writes OUTDIR/smk.html, OUTDIR/smk/organvave-1-smk37[-elite|-mke]-vVERSION.fwsc and OUTDIR/smk/index.html (to smk.html).
# Each package must carry its model's identity (SMK-37 Pro_8XY, SMK-37 Elite_8XY, MKE-P37_8XY) and the Felucca loader.
# VERSION must be ORGANVAVE_VERSION in firmware/src/core.h. (The site's index.html, the FM-1's: web/make_site.py.)
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from make_site import ROOT, product_of, strip_module   # noqa: E402

MODELS = [("SMK-37 Pro", ""), ("SMK-37 Elite", "-elite"), ("MKE-P37", "-mke")]


def main(outdir, version, *pkgs):
    if len(pkgs) != 3:
        raise SystemExit(__doc__ if __doc__ else "usage: make_smk_site.py OUTDIR VERSION PRO ELITE MKE")
    core = (ROOT / "firmware" / "src" / "core.h").read_text(encoding="utf-8")
    if f'#define ORGANVAVE_VERSION "v{version}"' not in core:
        raise SystemExit(f"VERSION {version} is not ORGANVAVE_VERSION in firmware/src/core.h")
    out = Path(outdir)
    (out / "smk").mkdir(parents=True, exist_ok=True)
    entries = []
    for (model, suffix), pkg in zip(MODELS, pkgs):
        raw = Path(pkg).read_bytes()
        product = product_of(raw)
        if not re.fullmatch(re.escape(model) + r"_8\d\d", product) or b"FELUCCA-LOADER-1" not in raw:
            raise SystemExit(f"{pkg}: not the {model} package of this firmware (identity {product!r})")
        file = f"organvave-1-smk37{suffix}-v{version}.fwsc"
        shutil.copyfile(pkg, out / "smk" / file)
        entries.append({"model": model, "product": product, "file": "smk/" + file})
    html = (HERE / "site_smk.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((HERE / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js"))
    for mark in ("/*LIB*/", "/*META*/{}"):
        if html.count(mark) != 1:
            raise SystemExit(f"site_smk.html must contain {mark} once")
    meta = json.dumps({"version": version, "pkgs": entries})
    html = html.replace("/*LIB*/", lib).replace("/*META*/{}", meta + ";").replace("/*VERSION*/", version)
    (out / "smk.html").write_text(html, encoding="utf-8")
    (out / "smk" / "index.html").write_text('<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=../smk.html">'
                                            '<title>OrganVave-1 for the SMK-37</title><a href="../smk.html">OrganVave-1 for the SMK-37</a>\n',
                                            encoding="utf-8")
    print(f"{out}/smk.html: {len(html)} B; " + ", ".join(e["product"] for e in entries) + " in smk/")


if __name__ == "__main__":
    main(*sys.argv[1:])
