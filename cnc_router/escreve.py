# -*- coding: utf-8 -*-
"""Escreve o roteamento de volta no board do KiCad.

Unico lugar, junto com `placa.py`, que fala a lingua do pcbnew. O resultado chega
aqui em milimetro e sai como PCB_TRACK e PCB_VIA de verdade, com rede, camada e
largura - para o KiCad tratar como trilha normal: DRC, seleciona, arrasta, desfaz.
"""
from __future__ import annotations

import pcbnew

from .grade import BAIXO, CIMA


def _nm(mm: float):
    return pcbnew.FromMM(float(mm))


def _camada(board, nome: str) -> int:
    return board.GetLayerID(nome)


def limpa(board, so_das_redes=None) -> int:
    """Tira as trilhas e vias que ja existem. Devolve quantas saiu.

    Mexe so em cobre: contorno, textos, zonas e footprints ficam como estao. Se
    `so_das_redes` vier preenchido, poupa o que for de outra rede - serve para
    rotear uma rede sem desfazer o resto.
    """
    fora = [t for t in board.Tracks()
            if so_das_redes is None or t.GetNetCode() in so_das_redes]
    for t in fora:
        board.Remove(t)
    return len(fora)


def move_pecas(board, placa) -> int:
    """Leva para o board a posicao e o giro que o posicionador escolheu.

    Faltava inteiro: o posicionador mexia no modelo e o resultado morria ali - o
    arquivo gravado tinha trilha nova com as pecas no lugar antigo. Peca travada
    nunca e tocada, nem que o modelo diga outra coisa: o cadeado do KiCad manda.
    """
    por_ref = {p.ref: p for p in placa.pecas}
    movidas = 0
    for fp in board.GetFootprints():
        p = por_ref.get(fp.GetReference())
        if p is None or fp.IsLocked():
            continue
        nova = pcbnew.VECTOR2I(_nm(p.x), _nm(p.y))
        if (fp.GetPosition() != nova
                or abs(fp.GetOrientationDegrees() - p.giro) > 1e-6):
            fp.SetPosition(nova)
            fp.SetOrientationDegrees(p.giro)
            movidas += 1
    return movidas


def escreve(board, resultado, regras) -> dict:
    """Cria no board as trilhas e vias do resultado."""
    largura = _nm(regras.trilha)
    via_ilha = _nm(regras.via_ilha)
    via_furo = _nm(regras.broca)

    criadas = {"trilhas": 0, "vias": 0}

    for (x1, y1, x2, y2, face, rede) in resultado.trilhas:
        if (x1, y1) == (x2, y2):
            continue        # segmento degenerado nao vira cobre
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(_nm(x1), _nm(y1)))
        t.SetEnd(pcbnew.VECTOR2I(_nm(x2), _nm(y2)))
        t.SetWidth(largura)
        t.SetLayer(_camada(board, face))
        t.SetNetCode(rede)
        board.Add(t)
        criadas["trilhas"] += 1

    for (x, y, rede) in resultado.vias:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(_nm(x), _nm(y)))
        v.SetWidth(via_ilha)
        v.SetDrill(via_furo)
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        # a via atravessa: vale das duas faces de cobre
        v.SetLayerPair(_camada(board, CIMA), _camada(board, BAIXO))
        v.SetNetCode(rede)
        board.Add(v)
        criadas["vias"] += 1

    board.BuildConnectivity()
    return criadas
