# -*- coding: utf-8 -*-
"""Monta o mapa em C a partir da placa lida e roteia. Cola, nao decide.

Existia espalhado em cada script de teste; virou um lugar so porque agora o
roteamento e chamado em laco, e repetir a montagem em cinco arquivos era pedir
para um deles divergir.
"""
from __future__ import annotations

import ctypes

from . import nativo
from .regras import Custos

FORMA = {"circulo": nativo.CIRCULO, "oval": nativo.CAPSULA}


class Resultado:
    def __init__(self, bruto, mapa, placa=None):
        self.ligadas = bruto["ligadas"]
        # O C devolve a ligacao aberta como (rede, coluna, linha) - numero que nao
        # diz nada a quem vai olhar a placa. Volta aqui para nome de rede e pad.
        self.abertas = []
        for rede, c, l in bruto["abertas"]:
            x, y = mapa.para_mm(c, l)
            pad = None
            if placa is not None:
                da_rede = [q for q in placa.pads if q.rede == rede]
                if da_rede:
                    pad = min(da_rede, key=lambda q: (q.x - x) ** 2 + (q.y - y) ** 2)
            self.abertas.append((pad.rede_nome if pad else "rede %d" % rede,
                                 pad.ref if pad else "(%.1f, %.1f)" % (x, y)))
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
        # a borda pede a folga dela, que pode ser maior que a folga entre redes
        formas.append(nativo.PbForma(forma=nativo.FORA_DE, rede=nativo.DE_NINGUEM,
                                     faces=nativo.AMBAS,
                                     folga_extra=max(0.0, regras.borda - regras.folga),
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
    m.define_planos(placa.planos)
    return m


def roteia(placa, regras, custos: Custos = None) -> Resultado:
    m = monta_mapa(placa, regras, custos)
    alvos = [(pad.rede,) + m.para_grade(pad.x, pad.y)
             for pad in placa.pads if pad.rede]
    return Resultado(m.roteia_tudo(alvos), m, placa)


def posiciona(placa, regras, segundos=120.0, candidatos=0, passos=3000, semente=1,
              nucleos=0, custos=None, placar=None, conectores_na_periferia=True):
    """Posiciona no C, com o roteador como juiz. Escreve a vencedora na placa.

    Sorteia em todos os nucleos ate fechar a placa inteira ou acabar `segundos`
    (`candidatos` > 0 poe teto tambem no numero de sorteios).

    `placar` (um nativo.PbPlacar) e opcional: quem passa o seu pode le-lo de outra
    thread enquanto o C trabalha, e por `placar.parar = 1` para encerrar.

    Peca travada (cadeado do KiCad) e peca mecanica (sem rede: furo de fixacao)
    nao se movem. Devolve o placar: abertas antes e depois, vias e fio.
    """
    lib = nativo.lib()
    margem = regras.dilatacao()
    rc = nativo.regras_para_c(regras, custos or Custos())

    com_rede = {pad.dono for pad in placa.pads if pad.rede}
    indice = {p.ref: i for i, p in enumerate(placa.pecas)}
    corpos = []
    for p in placa.pecas:
        v = (ctypes.c_double * max(2, len(p.corpo) * 2))()
        for k, (x, y) in enumerate(p.corpo):
            v[k * 2], v[k * 2 + 1] = x, y
        corpos.append(v)
    pecas = (nativo.PbPeca * len(placa.pecas))(*[
        nativo.PbPeca(x=p.x, y=p.y, giro=p.giro, larg=p.largura, alt=p.altura,
                      cx=p.cx, cy=p.cy,
                      movel=int(not p.travada and p.ref in com_rede),
                      n_corpo=len(p.corpo), corpo=corpos[i],
                      papel=(nativo.PAPEL_MECANICA if p.ref not in com_rede
                             else nativo.PAPEL_CONECTOR
                             if conectores_na_periferia and p.conector
                             else nativo.PAPEL_COMUM))
        for i, p in enumerate(placa.pecas)])
    pads = (nativo.PbPadC * len(placa.pads))(*[
        nativo.PbPadC(peca=indice[pad.dono], rede=pad.rede,
                      forma=FORMA.get(pad.forma, nativo.RETANGULO),
                      dx=pad.dx, dy=pad.dy, giro_local=pad.giro_local,
                      larg=pad.largura, alt=pad.altura)
        for pad in placa.pads])

    contorno = (ctypes.c_double * max(2, len(placa.contorno) * 2))()
    for i, (x, y) in enumerate(placa.contorno):
        contorno[i * 2], contorno[i * 2 + 1] = x, y

    vivos = []
    # Area que mora num footprint MOVEL anda com ele, e o posicionador em C so
    # conhece area parada: fica de fora do sorteio (o juiz roteia um pouco
    # otimista ali) e volta, no lugar novo, para o roteamento final.
    moveis = {p.ref for p in placa.pecas if not p.travada and p.ref in com_rede}
    paradas = [z for z in placa.proibidas if z.dono not in moveis]
    proibidas = (nativo.PbProibida * max(1, len(paradas)))()
    for i, z in enumerate(paradas):
        pts = (ctypes.c_double * (len(z.poligono) * 2))()
        for k, (x, y) in enumerate(z.poligono):
            pts[k * 2], pts[k * 2 + 1] = x, y
        vivos.append(pts)
        faces = 0
        if z.sem_trilha:
            faces = ((nativo.CIMA if "F.Cu" in z.camadas else 0)
                     | (nativo.BAIXO if "B.Cu" in z.camadas else 0))
        proibidas[i] = nativo.PbProibida(faces_trilha=faces,
                                         sem_footprint=int(z.sem_footprint),
                                         n_pontos=len(z.poligono), pontos=pts)

    planos = nativo.planos_para_c(placa.planos)
    if placar is None:
        placar = nativo.PbPlacar()
    lib.pb_posiciona(ctypes.byref(rc),
                     placa.x0 - margem, placa.y0 - margem,
                     placa.largura + 2 * margem, placa.altura + 2 * margem,
                     contorno, len(placa.contorno),
                     proibidas, len(paradas),
                     planos, planos._n,
                     pecas, len(placa.pecas), pads, len(placa.pads),
                     candidatos, passos, semente, float(segundos), nucleos,
                     ctypes.byref(placar))

    # a vencedora volta para a placa: pecas e, por consequencia, pads
    for i, p in enumerate(placa.pecas):
        p.x, p.y, p.giro = pecas[i].x, pecas[i].y, pecas[i].giro
    import math
    por_ref = {p.ref: p for p in placa.pecas}
    for pad in placa.pads:
        p = por_ref[pad.dono]
        a = math.radians(-p.giro)    # local -> mundo: -giro (convencao em placa.py)
        pad.x = p.x + pad.dx * math.cos(a) - pad.dy * math.sin(a)
        pad.y = p.y + pad.dx * math.sin(a) + pad.dy * math.cos(a)
        pad.giro = p.giro + pad.giro_local
    for z in placa.proibidas:
        if z.dono in moveis:
            p = por_ref[z.dono]
            a = math.radians(-p.giro)
            z.poligono = [(p.x + x * math.cos(a) - y * math.sin(a),
                           p.y + x * math.sin(a) + y * math.cos(a)) for x, y in z.local]
    return placar
