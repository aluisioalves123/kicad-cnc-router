# -*- coding: utf-8 -*-
"""Roteia a placa inteira: todas as redes, com disputa de espaco.

A ordem importa. Quem roteia primeiro escolhe o caminho bom e deixa o resto do
espaco para os outros, entao as redes dificeis vao primeiro - dificil aqui e "tem
muitos pads" e "os pads estao longe uns dos outros". Deixar uma rede de 6 pads por
ultimo e pedir para ela nao fechar.

Cada rede, depois de roteada, e PINTADA no mapa. As seguintes passam a desviar
dela, e a propria rede ganha o cobre inteiro como ponto de partida: o proximo
trecho pode sair do meio de uma trilha, nao so de um pad. E assim que se liga uma
rede de verdade, em arvore, e nao em corrente.
"""
from __future__ import annotations

import collections
import math

from .grade import BAIXO, CIMA
from .mapa import Mapa
from .roteador import Custos, Roteador, em_segmentos


class Resultado:
    def __init__(self):
        self.trilhas = []       # (x1, y1, x2, y2, face, rede)
        self.vias = []          # (x, y, rede)
        self.abertas = []       # (rede_nome, de, ate) que nao fecharam
        self.ligadas = 0

    @property
    def ok(self):
        return not self.abertas


def _distancia(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def _dificuldade(pads):
    """Redes mais dificeis primeiro: muitos pads, e espalhados."""
    if len(pads) < 2:
        return (0, 0.0)
    espalhamento = max(_distancia(a, b) for a in pads for b in pads)
    return (-len(pads), -espalhamento)


class Projeto:
    def __init__(self, placa, regras, custos: Custos = None):
        self.placa = placa
        self.regras = regras
        self.mapa = Mapa(placa, regras)
        self.roteador = Roteador(self.mapa, custos)

    def roteia_tudo(self, ao_progredir=None) -> Resultado:
        res = Resultado()
        redes = collections.defaultdict(list)
        for pad in self.placa.pads:
            if pad.rede:
                redes[pad.rede].append(pad)

        ordem = sorted((r for r in redes.items() if len(r[1]) > 1),
                       key=lambda kv: _dificuldade(kv[1]))

        for i, (rede, pads) in enumerate(ordem):
            if ao_progredir:
                ao_progredir(i, len(ordem), pads[0].rede_nome)
            self._roteia_rede(rede, pads, res)
        return res

    # ------------------------------------------------------------------
    def _roteia_rede(self, rede, pads, res: Resultado):
        """Liga os pads de uma rede em arvore, sempre pelo trecho mais barato."""
        ligados = [pads[0]]
        # O ponto de partida cresce: comeca no primeiro pad e vai ganhando todo o
        # cobre que a rede for criando.
        origens = {self.mapa.chegada_do_pad(pads[0])}
        faltam = list(pads[1:])

        while faltam:
            # proximo alvo: o pad mais perto do que ja esta ligado
            alvo = min(faltam, key=lambda p: min(_distancia(p, q) for q in ligados))
            caminho = self.roteador.liga(rede, origens,
                                         [self.mapa.chegada_do_pad(alvo)])
            if caminho is None:
                res.abertas.append((alvo.rede_nome, ligados[0].ref, alvo.ref))
                faltam.remove(alvo)
                continue

            segmentos, vias = em_segmentos(caminho, self.mapa)
            self._guarda(segmentos, vias, rede, res)
            self._pinta(segmentos, vias, rede)
            # o mapa mudou: o que o roteador sabia sobre onde cabe via envelheceu
            self.roteador._via_cabe.clear()

            # todo o caminho vira ponto de partida para o proximo trecho
            for (c, l, face, _d) in caminho:
                origens.add((c, l, face))
            ligados.append(alvo)
            faltam.remove(alvo)
            res.ligadas += 1

    def _guarda(self, segmentos, vias, rede, res: Resultado):
        g = self.mapa.faces[BAIXO]
        for (a, b) in segmentos:
            x1, y1 = g.para_mm(a[0], a[1])
            x2, y2 = g.para_mm(b[0], b[1])
            res.trilhas.append((x1, y1, x2, y2, a[2], rede))
        for v in vias:
            x, y = g.para_mm(v[0], v[1])
            res.vias.append((x, y, rede))

    def _pinta(self, segmentos, vias, rede):
        """O cobre novo entra no mapa: as proximas redes desviam dele."""
        folga = self.regras.dilatacao()
        raio = self.regras.trilha / 2.0 + folga
        g0 = self.mapa.faces[BAIXO]
        for (a, b) in segmentos:
            x1, y1 = g0.para_mm(a[0], a[1])
            x2, y2 = g0.para_mm(b[0], b[1])
            self.mapa.faces[a[2]].pinta_segmento(x1, y1, x2, y2, raio, rede)
        raio_via = self.regras.via_ilha / 2.0 + folga
        for v in vias:
            x, y = g0.para_mm(v[0], v[1])
            for face in self.mapa.faces.values():
                face.pinta_circulo(x, y, raio_via, rede)
