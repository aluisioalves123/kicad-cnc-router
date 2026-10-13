# -*- coding: utf-8 -*-
"""Monta as duas faces rasterizadas a partir da placa lida.

Aqui mora a regra que motivou o projeto inteiro:

    NA FACE DE CIMA, O PAD DE COMPONENTE E OBSTACULO ATE PARA A PROPRIA REDE.

O pad existe nas duas faces - no KiCad ele aparece em F.Cu e B.Cu. Mas o cobre de
cima e o de baixo so ficam ligados DEPOIS de soldados, e soldar por cima e
exatamente o que nao se quer fazer. Entao uma trilha que chega no pad por cima nao
e uma ligacao: e uma solda a mais no lado errado da placa.

Na placa blink blink v4, feita a mao, isso aconteceu 11 vezes. Com esta regra o
roteador e obrigado a descer de via e fazer o ultimo trecho por baixo - que e o que
se faz na bancada de qualquer jeito.
"""
from __future__ import annotations

from .grade import BAIXO, CIMA, DE_NINGUEM, FACES, Grade


def _pinta_pad(g: Grade, pad, folga: float, rede: int) -> None:
    """Pinta um pad ja inflado pela folga pedida."""
    if pad.forma == "circulo":
        g.pinta_circulo(pad.x, pad.y, pad.largura / 2.0 + folga, rede)
    elif pad.forma == "oval":
        g.pinta_capsula(pad.x, pad.y, pad.largura + 2 * folga,
                        pad.altura + 2 * folga, pad.giro, rede)
    else:
        g.pinta_retangulo_girado(pad.x, pad.y, pad.largura + 2 * folga,
                                 pad.altura + 2 * folga, pad.giro, rede)


class Mapa:
    """As duas faces da placa, prontas para o roteador perguntar."""

    def __init__(self, placa, regras):
        self.placa = placa
        self.regras = regras
        self.faces = {}

        margem = regras.dilatacao()
        for face in FACES:
            self.faces[face] = Grade(placa.x0 - margem, placa.y0 - margem,
                                     placa.largura + 2 * margem,
                                     placa.altura + 2 * margem, regras.grade)

        self._pinta_contorno()
        self._pinta_proibidas()
        self._pinta_pads()
        self._pinta_cobre_existente()

    # ------------------------------------------------------------------
    def _pinta_contorno(self):
        """Fora da placa e de ninguem, e a borda tambem: o cobre precisa caber."""
        if not self.placa.contorno:
            return
        recuo = self.regras.trilha / 2.0 + self.regras.folga
        for g in self.faces.values():
            g.pinta_fora_do_poligono(self.placa.contorno, DE_NINGUEM, recuo)

    def _pinta_proibidas(self):
        """Area de regra do KiCad, so nas camadas e com as proibicoes dela."""
        folga = self.regras.dilatacao()
        for z in self.placa.proibidas:
            if not z.sem_trilha:
                continue        # ha area que barra so via ou so cobre
            for nome in z.camadas:
                g = self.faces.get(nome)
                if g is not None:
                    g.pinta_poligono(z.poligono, DE_NINGUEM, folga)

    def _pinta_pads(self):
        folga = self.regras.dilatacao()
        for pad in self.placa.pads:
            # Embaixo o pad e da rede dele: a trilha da mesma rede pode chegar.
            _pinta_pad(self.faces[BAIXO], pad, folga, pad.rede)
            # Em cima ele e intransponivel ATE para a propria rede, senao a trilha
            # encostaria no anel e isso so vira ligacao com solda por cima.
            _pinta_pad(self.faces[CIMA], pad, folga, DE_NINGUEM)

    def _pinta_cobre_existente(self):
        """Trilha e via que ja estavam na placa continuam valendo como obstaculo."""
        folga = self.regras.dilatacao()
        for x1, y1, x2, y2, larg, camada, rede in self.placa.trilhas:
            g = self.faces.get(camada)
            if g is not None:
                g.pinta_segmento(x1, y1, x2, y2, larg / 2.0 + folga, rede)
        for x, y, larg, _furo, rede in self.placa.vias:
            for g in self.faces.values():
                g.pinta_circulo(x, y, larg / 2.0 + folga, rede)

    # ------------------------------------------------------------------
    def chegada_do_pad(self, pad):
        """Celula por onde a trilha entra neste pad, e em que face.

        Sempre por baixo - essa e a regra. Devolve (coluna, linha, face).
        """
        g = self.faces[BAIXO]
        return g.para_grade(pad.x, pad.y) + (BAIXO,)

    def resumo(self):
        return {face: g.conta() for face, g in self.faces.items()}
