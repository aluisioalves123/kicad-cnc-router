# -*- coding: utf-8 -*-
"""O DRC do proprio KiCad como juiz final: o que a placa gerada tem de NOVO.

Existe porque o conferidor deste projeto, por mais que meça por fora, mede o que
eu pensei em medir. O DRC do KiCad pegou dois defeitos que ele nao via - uma peca
dentro de area proibida e um courtyard cruzado. Entao toda rodada compara as
violacoes da placa gerada com as da original, e o que importa e so a diferenca:
a placa de entrada pode ter violacao propria (furo de fixacao no canto, silk sobre
cobre) e isso nao e culpa do roteador.
"""
from __future__ import annotations

import collections
import json
import os
import subprocess
import tempfile

KICAD_CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"

# Cosmetico ou de biblioteca: nao dizem nada sobre o que o roteador fez.
IGNORA = {"silk_overlap", "silk_over_copper", "lib_footprint_mismatch",
          "silk_edge_clearance", "text_height", "text_thickness"}


def _violacoes(caminho):
    if not os.path.exists(KICAD_CLI):
        return None
    saida = os.path.join(tempfile.gettempdir(), "cnc_router_drc.json")
    # --refill-zones: com plano, o que conta e o plano preenchido para AS
    # trilhas que estao la, nao o desenho de quando a placa foi salva
    subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--format", "json",
                    "--output", saida, caminho],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(saida, encoding="utf-8") as f:
        d = json.load(f)
    chaves = collections.Counter()
    for v in d.get("violations", []):
        if v["type"] in IGNORA:
            continue
        itens = " | ".join(sorted(i["description"] for i in v["items"]))
        chaves[(v["type"], itens)] += 1
    return chaves, len(d.get("unconnected_items", []))


def novas(original, gerada):
    """Violacoes que a placa gerada tem e a original nao tinha."""
    a, b = _violacoes(original), _violacoes(gerada)
    if a is None or b is None:
        return None, None
    return sorted((b[0] - a[0]).elements()), (a[1], b[1])
