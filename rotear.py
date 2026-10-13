# -*- coding: utf-8 -*-
"""Roteia um .kicad_pcb e grava o resultado AO LADO do original.

Grava em arquivo novo de proposito: enquanto o roteador nao prestou contas numa
placa que voce de fato fresou, o seu arquivo nao deve correr risco. Quem compara
os dois no KiCad e voce.

    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" rotear.py placa.kicad_pcb
"""
from __future__ import annotations

import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pcbnew  # noqa: E402

from cnc_router import escreve as escritor  # noqa: E402
from cnc_router import placa as leitor  # noqa: E402
from cnc_router.confere import confere  # noqa: E402
from cnc_router import motor  # noqa: E402
from cnc_router.regras import Regras  # noqa: E402


def principal(caminho: str) -> int:
    regras = Regras()
    board = pcbnew.LoadBoard(caminho)

    p = leitor.ler(board)
    regras.do_projeto(p)            # folga de borda e entre redes: o projeto manda
    print("folga entre redes %.2f mm, cobre-borda %.2f mm" % (regras.folga, regras.borda))
    antes = len(p.trilhas), len(p.vias)
    # roteia do zero: o que estava la e descartado do modelo
    p.trilhas, p.vias = [], []

    t0 = time.time()
    placar = motor.posiciona(p, regras)
    print("posicionado em %.1fs: abertas %d -> %d (candidato %d; %d sorteios "
          "roteados, %d ilegais descartados, %d nucleos)"
          % (time.time() - t0, placar.abertas_antes, placar.abertas_depois,
             placar.vencedor, placar.julgados, placar.descartados, placar.nucleos))

    t0 = time.time()
    res = motor.roteia(p, regras)
    print("roteado em %.2fs" % (time.time() - t0))

    print()
    print("RESULTADO")
    print("  ligacoes feitas ... %d" % res.ligadas)
    print("  trilhas ........... %d  (antes havia %d)" % (len(res.trilhas), antes[0]))
    print("  vias .............. %d  (antes havia %d)" % (len(res.vias), antes[1]))
    print("  por face .......... %s"
          % dict(collections.Counter(t[4] for t in res.trilhas)))
    if res.abertas:
        print("  NAO fecharam ...... %d" % len(res.abertas))
        for rede, pad in res.abertas:
            print("     %s: falta chegar em %s" % (rede, pad))

    print()
    print("CONFERENCIA (medida em mm, sem olhar a grade)")
    problemas = confere(p, regras, res)
    for x in problemas:
        print("  ! " + x)
    if not problemas:
        print("  nenhum problema")

    movidas = escritor.move_pecas(board, p)
    tirou = escritor.limpa(board)
    criou = escritor.escreve(board, res, regras)
    saida = caminho.replace(".kicad_pcb", "-roteado.kicad_pcb")
    board.Save(saida)

    print()
    print("GRAVADO")
    print("  moveu %d peca(s); tirou %d trilha(s)/via(s) antigas, criou %d trilhas e %d vias"
          % (movidas, tirou, criou["trilhas"], criou["vias"]))
    print("  %s" % saida)

    from cnc_router import drc
    print()
    print("DRC DO KICAD (so o que a placa gerada tem de NOVO)")
    lista, sem_ligacao = drc.novas(caminho, saida)
    if lista is None:
        print("  kicad-cli nao encontrado: DRC nao rodou")
    else:
        print("  sem ligacao: original %d, gerada %d" % sem_ligacao)
        for tipo, itens in lista:
            print("  ! %s: %s" % (tipo, itens))
        if not lista:
            print("  nenhuma violacao nova")
    return 1 if (problemas or lista) else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    raise SystemExit(principal(sys.argv[1]))
