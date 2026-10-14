# -*- coding: utf-8 -*-
"""Mede a placa gerada e cobra as regras, sem olhar a grade.

Existe porque o mapa e o roteador sao a MESMA fonte de verdade: se a rasterizacao
estiver errada, o roteador erra junto e com confianca, e a conferencia feita na
grade concordaria com os dois. Aqui a conta e refeita em milimetro, na geometria
de verdade - distancia entre segmentos, distancia ate pad, angulo real.

E o mesmo raciocinio do `montagem_no_papel` do roteador de perfboard: so vale como
conferencia o que for medido por fora.
"""
from __future__ import annotations

import math

from .regras import BAIXO, CIMA


def _dist_segmentos(a, b) -> float:
    """Menor distancia entre dois segmentos no plano."""
    (x1, y1, x2, y2), (x3, y3, x4, y4) = a, b
    if _cruzam(a, b):
        return 0.0
    return min(_dist_ponto_segmento(x1, y1, x3, y3, x4, y4),
               _dist_ponto_segmento(x2, y2, x3, y3, x4, y4),
               _dist_ponto_segmento(x3, y3, x1, y1, x2, y2),
               _dist_ponto_segmento(x4, y4, x1, y1, x2, y2))


def _cruzam(a, b) -> bool:
    (x1, y1, x2, y2), (x3, y3, x4, y4) = a, b

    def lado(xa, ya, xb, yb, xc, yc):
        v = (xb - xa) * (yc - ya) - (yb - ya) * (xc - xa)
        return (v > 1e-12) - (v < -1e-12)

    d1 = lado(x1, y1, x2, y2, x3, y3)
    d2 = lado(x1, y1, x2, y2, x4, y4)
    d3 = lado(x3, y3, x4, y4, x1, y1)
    d4 = lado(x3, y3, x4, y4, x2, y2)
    return d1 != d2 and d3 != d4


def _dist_ponto_segmento(px, py, x1, y1, x2, y2) -> float:
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def _ponto_mais_perto(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return x1, y1
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return x1 + t * dx, y1 + t * dy


def _raio_rumo_a(pad, x, y) -> float:
    """Quanto o pad avanca na direcao do ponto (x, y).

    A direcao certa e do PAD PARA A TRILHA, nao a direcao da trilha. Medir pelo
    rumo do segmento foi o erro que fez este conferidor acusar dois encostoes que
    nao existiam: num oval de 1,8 x 3 ao lado de um segmento vertical, o rumo do
    segmento aponta para o eixo de 3,0 enquanto quem aponta para a trilha e o de
    1,8 - quase um milimetro de diferenca.
    """
    import math as _m
    return pad.raio_na_direcao(_m.atan2(y - pad.y, x - pad.x))


def _angulo_ok(x1, y1, x2, y2, tolerancia=1.0) -> bool:
    """O segmento esta em 0, 45 ou 90 graus?"""
    if x1 == x2 and y1 == y2:
        return True
    ang = math.degrees(math.atan2(y2 - y1, x2 - x1)) % 45.0
    return min(ang, 45.0 - ang) <= tolerancia


def confere(placa, regras, resultado) -> list:
    """Devolve a lista de problemas medidos. Vazia = placa limpa."""
    problemas = []
    meia = regras.trilha / 2.0

    # ---------------------------------------------------------------- 45 graus
    tortas = [t for t in resultado.trilhas if not _angulo_ok(t[0], t[1], t[2], t[3])]
    if tortas:
        problemas.append("%d trilha(s) fora de 0/45/90 graus, ex.: (%.2f,%.2f)-(%.2f,%.2f)"
                         % (len(tortas), tortas[0][0], tortas[0][1], tortas[0][2], tortas[0][3]))

    # ------------------------------------------- trilha de cima terminando em pad
    # E a regra que motiva o projeto: cobre de cima encostando num pad so vira
    # ligacao depois de soldar por cima, que e o que se quer eliminar.
    encostam = []
    for (x1, y1, x2, y2, face, rede) in resultado.trilhas:
        for pad in placa.pads:
            # a face da solda e a oposta ao corpo: embaixo, ou em cima se a peca
            # esta virada. Cobre na OUTRA face encostando no pad e o proibido.
            if face == (CIMA if pad.lado else BAIXO):
                continue
            qx, qy = _ponto_mais_perto(pad.x, pad.y, x1, y1, x2, y2)
            d = math.hypot(qx - pad.x, qy - pad.y)
            if d < _raio_rumo_a(pad, qx, qy) + meia - 1e-6:
                encostam.append((pad.ref, rede))
    if encostam:
        problemas.append("%d encostao(oes) de trilha em pad pelo lado do corpo: %s"
                         % (len(encostam), sorted({e[0] for e in encostam})[:6]))

    # ---------------------------------------------------------------- folga
    perto = []
    for i, a in enumerate(resultado.trilhas):
        for b in resultado.trilhas[i + 1:]:
            if a[4] != b[4] or a[5] == b[5]:
                continue        # faces diferentes nao se veem; mesma rede pode tocar
            d = _dist_segmentos(a[:4], b[:4]) - regras.trilha
            if d < regras.folga - 1e-6:
                perto.append((round(d, 3), a[5], b[5]))
    if perto:
        perto.sort()
        problemas.append("%d par(es) de trilha de redes diferentes perto demais; "
                         "pior %.3f mm (minimo %.2f)" % (len(perto), perto[0][0], regras.folga))

    # ------------------------------------------------- trilha perto de pad alheio
    perto_pad = []
    for (x1, y1, x2, y2, face, rede) in resultado.trilhas:
        for pad in placa.pads:
            if pad.rede == rede:
                continue
            qx, qy = _ponto_mais_perto(pad.x, pad.y, x1, y1, x2, y2)
            d = math.hypot(qx - pad.x, qy - pad.y)
            limite = _raio_rumo_a(pad, qx, qy) + meia + regras.folga
            if d < limite - 1e-6:
                perto_pad.append((round(limite - d, 3), pad.ref, rede))
    if perto_pad:
        perto_pad.sort(reverse=True)
        problemas.append("%d trilha(s) perto demais de pad de outra rede; pior invade "
                         "%.3f mm (ex.: %s)" % (len(perto_pad), perto_pad[0][0], perto_pad[0][1]))

    # ---------------------------------------------------------------- vias
    # Faltava inteiro: o conferidor olhava trilha contra trilha e trilha contra
    # pad, e nunca olhava via. Deu "nenhum problema" numa placa com duas vias de
    # redes diferentes sobrepostas - curto que so apareceu no olho de quem ia
    # fresar. Via e cobre como qualquer outro, e furo alem disso.
    raio_via = regras.via_ilha / 2.0

    for i, (xa, ya, ra) in enumerate([(v[0], v[1], v[2]) for v in resultado.vias]):
        for (xb, yb, rb) in [(v[0], v[1], v[2]) for v in resultado.vias][i + 1:]:
            d = math.hypot(xb - xa, yb - ya)
            # mesma rede pode encostar o cobre, mas furo com furo nunca: a broca
            # arrebenta a parede entre dois furos coladas
            minimo = (regras.broca + regras.folga if ra == rb
                      else 2 * raio_via + regras.folga)
            if d < minimo - 1e-6:
                problemas.append("duas vias a %.2f mm (minimo %.2f) - %s"
                                 % (d, minimo,
                                    "mesma rede" if ra == rb else "REDES DIFERENTES: curto"))

    for (x, y, rede) in resultado.vias:
        for pad in placa.pads:
            d = math.hypot(pad.x - x, pad.y - y)
            folga_aqui = 0.0 if pad.rede == rede else regras.folga
            limite = _raio_rumo_a(pad, x, y) + raio_via + folga_aqui
            if d < limite - 1e-6:
                problemas.append("via em (%.2f,%.2f) invade o pad %s em %.2f mm"
                                 % (x, y, pad.ref, limite - d))
        for (x1, y1, x2, y2, face, rede_t) in resultado.trilhas:
            if rede_t == rede:
                continue
            d = _dist_ponto_segmento(x, y, x1, y1, x2, y2)
            limite = raio_via + regras.trilha / 2.0 + regras.folga
            if d < limite - 1e-6:
                problemas.append("via em (%.2f,%.2f) perto demais de trilha da rede %s"
                                 % (x, y, rede_t))

    return problemas
