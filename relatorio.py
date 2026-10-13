# -*- coding: utf-8 -*-
"""Le uma placa e conta o que ela tem, pelo python do KiCad.

Roda fora do pcbnew de proposito: enquanto o roteador nao existe, este e o jeito de
exercitar a leitura contra placas reais sem abrir a interface.

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" relatorio.py caminho.kicad_pcb
"""
from __future__ import annotations

import collections
import itertools
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pcbnew  # noqa: E402

from cnc_router import placa as leitor  # noqa: E402
from cnc_router.regras import Regras  # noqa: E402


def vao_entre(a, b) -> float:
    """Vao livre entre dois pads, na direcao que liga os centros."""
    dx, dy = b.x - a.x, b.y - a.y
    d = math.hypot(dx, dy)
    if d == 0:
        return 0.0
    ang = math.atan2(dy, dx)
    return d - a.raio_na_direcao(ang) - b.raio_na_direcao(ang + math.pi)


def principal(caminho: str) -> int:
    r = Regras()
    p = leitor.ler(pcbnew.LoadBoard(caminho))

    print("PLACA: %s" % p.nome)
    print("  contorno .......... %.1f x %.1f mm (%d pontos)"
          % (p.largura, p.altura, len(p.contorno)))
    print("  pads .............. %d" % len(p.pads))
    print("  pecas ............. %d (%d travadas)"
          % (len(p.pecas), sum(1 for x in p.pecas if x.travada)))
    print("  areas proibidas ... %d" % len(p.proibidas))
    for z in p.proibidas:
        barra = ", ".join(n for n, v in (("trilha", z.sem_trilha), ("via", z.sem_via),
                                         ("cobre", z.sem_cobre)) if v)
        print("     %d pontos | camadas: %s | barra: %s"
              % (len(z.poligono), "+".join(z.camadas) or "nenhuma", barra or "nada"))
    print("  trilhas ........... %d" % len(p.trilhas))
    print("  vias .............. %d" % len(p.vias))
    print("  ligacoes abertas .. %d" % p.abertas)

    formas = collections.Counter(
        (x.forma, round(x.largura, 2), round(x.altura, 2), round(x.furo, 2))
        for x in p.pads)
    print()
    print("PADS")
    for (forma, larg, alt, furo), n in sorted(formas.items(), key=lambda kv: -kv[1]):
        print("  %3d x %-10s %sx%s mm, furo %s" % (n, forma, larg, alt, furo or "-"))

    por_camada = collections.Counter(t[5] for t in p.trilhas)
    larguras = collections.Counter(round(t[4], 2) for t in p.trilhas)
    if p.trilhas:
        print()
        print("TRILHAS JA DESENHADAS")
        print("  por camada: %s" % dict(por_camada))
        print("  larguras:   %s" % dict(larguras))

    # O numero que decide o roteamento: onde cabe trilha e onde nao cabe.
    print()
    print("ONDE CABE TRILHA  (precisa de %.2f mm: %.1f de trilha + 2 x %.1f de folga)"
          % (r.vao_necessario(), r.trilha, r.folga))
    vaos = []
    for a, b in itertools.combinations(p.pads, 2):
        if a.rede == b.rede and a.rede != 0:
            continue
        vaos.append((vao_entre(a, b), a.ref, b.ref))
    vaos.sort()
    apertados = [v for v in vaos if v[0] < r.vao_necessario()]
    print("  %d pares de pads de redes diferentes" % len(vaos))
    print("  %d deles SEM espaco para passar trilha no meio" % len(apertados))
    for v, na, nb in apertados[:6]:
        print("     %-10s <-> %-10s  vao %.2f mm" % (na, nb, v))
    if len(apertados) > 6:
        print("     ... e mais %d" % (len(apertados) - 6))

    # A regra que motiva o plugin: trilha de cima nao pode morrer em pad.
    print()
    print("TRILHA DE CIMA TERMINANDO EM PAD  (o que obriga solda por cima)")
    topo = [t for t in p.trilhas if t[5] == "F.Cu"]
    encostam = set()
    for t in topo:
        for px, py in ((t[0], t[1]), (t[2], t[3])):
            for pad in p.pads:
                if math.hypot(pad.x - px, pad.y - py) < 0.35:
                    encostam.add(pad.ref)
    print("  %d trilhas em F.Cu, encostando em %d pads" % (len(topo), len(encostam)))
    if encostam:
        print("  %s" % ", ".join(sorted(encostam)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    raise SystemExit(principal(sys.argv[1]))
