# -*- coding: utf-8 -*-
"""Monta o mapa em C a partir da placa lida e roteia. Cola, nao decide.

Existia espalhado em cada script de teste; virou um lugar so porque agora o
roteamento e chamado em laco, e repetir a montagem em cinco arquivos era pedir
para um deles divergir.
"""
from __future__ import annotations

import ctypes

from . import nativo
from .roteador import Custos

FORMA = {"circulo": nativo.CIRCULO, "oval": nativo.CAPSULA}


class Resultado:
    def __init__(self, bruto, mapa):
        self.ligadas = bruto["ligadas"]
        self.abertas = bruto["abertas"]
        self.trilhas = [(mapa.para_mm(t[0], t[1])[0], mapa.para_mm(t[0], t[1])[1],
                         mapa.para_mm(t[2], t[3])[0], mapa.para_mm(t[2], t[3])[1],
                         "F.Cu" if t[4] == 0 else "B.Cu", t[5])
                        for t in bruto["trilhas"]]
        self.vias = [(mapa.para_mm(v[0], v[1])[0], mapa.para_mm(v[0], v[1])[1], v[2])
                     for v in bruto["vias"]]

    @property
    def ok(self):
        return not self.abertas


def monta_mapa(placa, regras, custos: Custos = None):
    """Rasteriza a placa no C e devolve o mapa pronto para rotear."""
    margem = regras.dilatacao()
    rc = nativo.regras_para_c(regras, custos or Custos())
    m = nativo.Mapa(rc, placa.x0 - margem, placa.y0 - margem,
                    placa.largura + 2 * margem, placa.altura + 2 * margem)

    formas = []

    if placa.contorno:
        pts = (ctypes.c_double * (len(placa.contorno) * 2))()
        for i, (x, y) in enumerate(placa.contorno):
            pts[i * 2], pts[i * 2 + 1] = x, y
        m.guarda(pts)
        formas.append(nativo.PbForma(forma=nativo.FORA_DE, rede=nativo.DE_NINGUEM,
                                     faces=nativo.AMBAS,
                                     n_pontos=len(placa.contorno), pontos=pts))

    for z in placa.proibidas:
        if not z.sem_trilha:
            continue        # ha area que barra so via ou so cobre
        pts = (ctypes.c_double * (len(z.poligono) * 2))()
        for i, (x, y) in enumerate(z.poligono):
            pts[i * 2], pts[i * 2 + 1] = x, y
        m.guarda(pts)
        faces = ((nativo.CIMA if "F.Cu" in z.camadas else 0)
                 | (nativo.BAIXO if "B.Cu" in z.camadas else 0))
        formas.append(nativo.PbForma(forma=nativo.POLIGONO, rede=nativo.DE_NINGUEM,
                                     faces=faces, n_pontos=len(z.poligono), pontos=pts))

    for pad in placa.pads:
        f = FORMA.get(pad.forma, nativo.RETANGULO)
        # Embaixo o pad e da rede dele: a trilha da mesma rede pode chegar.
        formas.append(nativo.PbForma(forma=f, rede=pad.rede, faces=nativo.BAIXO,
                                     x=pad.x, y=pad.y, larg=pad.largura,
                                     alt=pad.altura, giro=pad.giro))
        # Em cima ele e intransponivel ATE para a propria rede: cobre de cima
        # encostando no anel so vira ligacao depois de soldar por cima, que e
        # exatamente o que este projeto existe para evitar.
        formas.append(nativo.PbForma(forma=f, rede=nativo.DE_NINGUEM,
                                     faces=nativo.CIMA,
                                     x=pad.x, y=pad.y, larg=pad.largura,
                                     alt=pad.altura, giro=pad.giro))

    for x1, y1, x2, y2, larg, camada, rede in placa.trilhas:
        formas.append(nativo.PbForma(forma=nativo.SEGMENTO, rede=rede,
                                     faces=(nativo.CIMA if camada == "F.Cu"
                                            else nativo.BAIXO),
                                     x=x1, y=y1, larg=x2, alt=y2, giro=larg))

    m.pinta(formas)
    return m


def roteia(placa, regras, custos: Custos = None) -> Resultado:
    m = monta_mapa(placa, regras, custos)
    alvos = [(pad.rede,) + m.para_grade(pad.x, pad.y)
             for pad in placa.pads if pad.rede]
    return Resultado(m.roteia_tudo(alvos), m)
