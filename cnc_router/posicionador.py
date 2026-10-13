# -*- coding: utf-8 -*-
"""Procura onde por as pecas para o roteamento caber.

A placa NAO muda de tamanho - isso e dado. O que o posicionador pode fazer e
arrumar as pecas dentro do contorno que existe.

O custo tem duas partes que puxam para lados opostos, e e essa briga que produz
layout bom:

* COMPRIMENTO: redes curtas, pelo semi-perimetro da caixa dos pads de cada rede.
  Sozinho ele amontoa tudo num canto - foi a licao do roteador de perfboard, onde
  minimizar fio deixava metade da placa vazia e a outra metade sem espaco para
  trabalhar.
* ESPALHAMENTO: a placa e dividida em regioes e paga-se caro por regiao lotada ao
  lado de regiao vazia. Aqui ele nao e so estetica: via de 2,5 mm precisa de
  3,3 mm de diametro livre, e esse espaco so existe se as pecas derem licenca.

E tres proibicoes, que nao sao caras e sim impossiveis: peca sobre peca, peca fora
do contorno e peca sobre area proibida.
"""
from __future__ import annotations

import math
import random

GIROS = (0.0, 90.0, 180.0, 270.0)

# Pesos. Sobreposicao e fuga do contorno sao proibicao, nao preferencia: o peso e
# alto de proposito, varias ordens acima do comprimento.
W_SOBREPOE = 500.0
W_FORA = 800.0
W_PROIBIDA = 800.0
W_ESPALHA = 0.9
REGIOES = 3


class Posicionador:
    def __init__(self, placa, regras, semente=1):
        self.placa = placa
        self.regras = regras
        self.rnd = random.Random(semente)
        self.moveis = [p for p in placa.pecas if not p.travada]
        self.pads_da_peca = {}
        for pad in placa.pads:
            self.pads_da_peca.setdefault(pad.dono, []).append(pad)
        self.redes = {}
        for pad in placa.pads:
            if pad.rede:
                self.redes.setdefault(pad.rede, []).append(pad)

    # ------------------------------------------------------------------ geometria
    @staticmethod
    def caixa(peca):
        """Retangulo da peca no mundo, ja considerando o giro."""
        larg, alt = peca.largura, peca.altura
        if int(peca.giro) % 180 == 90:
            larg, alt = alt, larg
        return (peca.x - larg / 2.0, peca.y - alt / 2.0,
                peca.x + larg / 2.0, peca.y + alt / 2.0)

    def posicoes_dos_pads(self, peca):
        a = math.radians(peca.giro)
        ca, sa = math.cos(a), math.sin(a)
        for pad in self.pads_da_peca.get(peca.ref, ()):
            yield pad, (peca.x + pad.dx * ca - pad.dy * sa,
                        peca.y + pad.dx * sa + pad.dy * ca)

    def aplica(self):
        """Grava nos pads a posicao que o giro e a posicao da peca implicam."""
        for peca in self.placa.pecas:
            a = math.radians(peca.giro)
            ca, sa = math.cos(a), math.sin(a)
            for pad in self.pads_da_peca.get(peca.ref, ()):
                pad.x = peca.x + pad.dx * ca - pad.dy * sa
                pad.y = peca.y + pad.dx * sa + pad.dy * ca
                pad.giro = peca.giro + pad.giro_local

    # ------------------------------------------------------------------ custo
    def custo(self) -> float:
        return (self._comprimento() + self._espalhamento()
                + W_SOBREPOE * self._sobreposicao()
                + W_FORA * self._fora_do_contorno()
                + W_PROIBIDA * self._sobre_area_proibida())

    def _comprimento(self) -> float:
        total = 0.0
        pos = {}
        for peca in self.placa.pecas:
            for pad, xy in self.posicoes_dos_pads(peca):
                pos[pad.ref] = xy
        for pads in self.redes.values():
            if len(pads) < 2:
                continue
            xs = [pos[p.ref][0] for p in pads]
            ys = [pos[p.ref][1] for p in pads]
            total += (max(xs) - min(xs)) + (max(ys) - min(ys))
        return total

    def _espalhamento(self) -> float:
        """Quanto a ocupacao esta desigual entre as regioes da placa."""
        area = [0.0] * (REGIOES * REGIOES)
        for peca in self.placa.pecas:
            x0, y0, x1, y1 = self.caixa(peca)
            cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
            c = min(REGIOES - 1, max(0, int((cx - self.placa.x0) * REGIOES / max(1e-6, self.placa.largura))))
            l = min(REGIOES - 1, max(0, int((cy - self.placa.y0) * REGIOES / max(1e-6, self.placa.altura))))
            area[l * REGIOES + c] += (x1 - x0) * (y1 - y0)
        media = sum(area) / len(area)
        return W_ESPALHA * sum((a - media) ** 2 for a in area) / len(area)

    def _sobreposicao(self) -> float:
        """Area de peca sobre peca. Conta a folga: duas pecas coladas nao montam."""
        folga = self.regras.folga
        total = 0.0
        caixas = [self.caixa(p) for p in self.placa.pecas]
        for i, a in enumerate(caixas):
            for b in caixas[i + 1:]:
                dx = min(a[2], b[2]) - max(a[0], b[0]) + folga
                dy = min(a[3], b[3]) - max(a[1], b[1]) + folga
                if dx > 0 and dy > 0:
                    total += dx * dy
        return total

    def _fora_do_contorno(self) -> float:
        """Peca passando da borda da placa.

        LIMITACAO CONHECIDA: mede contra o RETANGULO do contorno, nao contra o
        poligono. Numa placa retangular - o caso de hoje - da no mesmo; numa placa
        em L, deixaria peca no recorte. Quando aparecer a primeira placa recortada,
        isto aqui precisa virar teste de ponto no poligono.
        """
        if not self.placa.contorno:
            return 0.0
        xs = [p[0] for p in self.placa.contorno]
        ys = [p[1] for p in self.placa.contorno]
        bx0, by0, bx1, by1 = min(xs), min(ys), max(xs), max(ys)
        total = 0.0
        for peca in self.placa.pecas:
            x0, y0, x1, y1 = self.caixa(peca)
            total += (max(0.0, bx0 - x0) + max(0.0, x1 - bx1)
                      + max(0.0, by0 - y0) + max(0.0, y1 - by1))
        return total

    def _sobre_area_proibida(self) -> float:
        total = 0.0
        for z in self.placa.proibidas:
            xs = [p[0] for p in z.poligono]
            ys = [p[1] for p in z.poligono]
            zx0, zy0, zx1, zy1 = min(xs), min(ys), max(xs), max(ys)
            for peca in self.placa.pecas:
                x0, y0, x1, y1 = self.caixa(peca)
                dx = min(x1, zx1) - max(x0, zx0)
                dy = min(y1, zy1) - max(y0, zy0)
                if dx > 0 and dy > 0:
                    total += dx * dy
        return total

    # ------------------------------------------------------------------ busca
    def procura(self, passos=20000, ao_progredir=None):
        """Recozimento simulado. Devolve o melhor custo encontrado."""
        if not self.moveis:
            return self.custo()

        melhor = atual = self.custo()
        guardado = self._foto()
        passo_max = max(self.placa.largura, self.placa.altura) / 4.0
        t0 = max(1.0, atual / max(1, len(self.moveis)) * 0.5)

        for i in range(passos):
            frac = i / passos
            t = t0 * (0.02 / t0) ** frac
            desfaz = self._mexe(passo_max * (1.0 - 0.8 * frac))
            novo = self.custo()
            d = novo - atual
            if d <= 0 or self.rnd.random() < math.exp(-d / max(1e-9, t)):
                atual = novo
                if novo < melhor:
                    melhor, guardado = novo, self._foto()
            else:
                desfaz()
            if ao_progredir and i % 2000 == 0:
                ao_progredir(i, passos, melhor)

        self._repoe(guardado)
        self.aplica()
        return melhor

    def _foto(self):
        return [(p.x, p.y, p.giro) for p in self.placa.pecas]

    def _repoe(self, foto):
        for peca, (x, y, g) in zip(self.placa.pecas, foto):
            peca.x, peca.y, peca.giro = x, y, g

    def _mexe(self, alcance):
        """Faz um movimento e devolve a funcao que o desfaz."""
        peca = self.rnd.choice(self.moveis)
        antes = (peca.x, peca.y, peca.giro)

        sorteio = self.rnd.random()
        if sorteio < 0.70:
            peca.x += self.rnd.uniform(-alcance, alcance)
            peca.y += self.rnd.uniform(-alcance, alcance)
        elif sorteio < 0.90:
            peca.giro = self.rnd.choice(GIROS)
        else:
            outra = self.rnd.choice(self.moveis)
            antes_outra = (outra.x, outra.y, outra.giro)
            peca.x, outra.x = outra.x, peca.x
            peca.y, outra.y = outra.y, peca.y

            def desfaz_troca():
                peca.x, peca.y, peca.giro = antes
                outra.x, outra.y, outra.giro = antes_outra
            return desfaz_troca

        # arredonda para a grade de posicionamento: posicao com 7 casas decimais
        # nao ajuda ninguem e atrapalha quem for conferir no KiCad
        peca.x = round(peca.x / 0.25) * 0.25
        peca.y = round(peca.y / 0.25) * 0.25

        def desfaz():
            peca.x, peca.y, peca.giro = antes
        return desfaz
