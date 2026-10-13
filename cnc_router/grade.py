# -*- coding: utf-8 -*-
"""A placa virada grade: onde o centro da trilha pode passar, e de quem e cada lugar.

A ideia que faz tudo funcionar e nao perguntar "ha espaco aqui?" durante o
roteamento. Em vez disso, cada obstaculo e pintado JA INFLADO de meia trilha mais a
folga. Depois disso a pergunta vira trivial: o centro da trilha pode ocupar
qualquer celula que nao esteja pintada contra ele.

Cada celula guarda de quem ela e:

    LIVRE (0)      ninguem reclama
    <numero>       cobre daquela rede; so aquela rede pode passar
    DE_NINGUEM(-1) duas redes diferentes disputam, ou e borda/area proibida

O "de ninguem" existe porque dilatacao de redes diferentes se sobrepoe. Guardar so
a ultima que pintou deixaria a outra entrar onde nao cabe - e o curto sairia na
fresa, nao na tela.

A grade NAO e imposta pela placa, como era na perfboard. Ela e escolha nossa, e e o
que permite 45 graus: andando nas 8 direcoes de uma grade fina, a diagonal E o
segmento a 45 graus. A 0,1 mm o erro geometrico fica em 0,05 mm, uma ordem de
grandeza abaixo do que a fresa resolve.
"""
from __future__ import annotations

import math

LIVRE = 0
DE_NINGUEM = -1

# As duas faces, pelo nome que o KiCad usa.
CIMA = "F.Cu"
BAIXO = "B.Cu"
FACES = (CIMA, BAIXO)


class Grade:
    """Uma face da placa, rasterizada."""

    def __init__(self, x0: float, y0: float, largura: float, altura: float, passo: float):
        self.x0, self.y0, self.passo = x0, y0, passo
        self.colunas = max(1, int(math.ceil(largura / passo)) + 1)
        self.linhas = max(1, int(math.ceil(altura / passo)) + 1)
        self.celulas = [LIVRE] * (self.colunas * self.linhas)

    # ------------------------------------------------------------------ coordenadas
    def para_grade(self, x: float, y: float):
        return (int(round((x - self.x0) / self.passo)),
                int(round((y - self.y0) / self.passo)))

    def para_mm(self, c: int, l: int):
        return (self.x0 + c * self.passo, self.y0 + l * self.passo)

    def dentro(self, c: int, l: int) -> bool:
        return 0 <= c < self.colunas and 0 <= l < self.linhas

    # ------------------------------------------------------------------ leitura
    def dono(self, c: int, l: int) -> int:
        if not self.dentro(c, l):
            return DE_NINGUEM
        return self.celulas[l * self.colunas + c]

    def livre_para(self, c: int, l: int, rede: int) -> bool:
        d = self.dono(c, l)
        return d == LIVRE or d == rede

    # ------------------------------------------------------------------ escrita
    def marca(self, c: int, l: int, rede: int) -> None:
        """Pinta uma celula, resolvendo disputa entre redes.

        Rede diferente ja marcada vira DE_NINGUEM em vez de sobrescrever: o cobre
        das duas esta perto demais dali, e deixar a ultima vencer abriria passagem
        para a primeira exatamente onde nao cabe.
        """
        if not self.dentro(c, l):
            return
        i = l * self.colunas + c
        atual = self.celulas[i]
        if atual == rede or atual == DE_NINGUEM:
            return
        self.celulas[i] = rede if atual == LIVRE else DE_NINGUEM

    def pinta_circulo(self, x: float, y: float, raio: float, rede: int) -> None:
        c0, l0 = self.para_grade(x - raio, y - raio)
        c1, l1 = self.para_grade(x + raio, y + raio)
        r2 = raio * raio
        for l in range(l0, l1 + 1):
            for c in range(c0, c1 + 1):
                mx, my = self.para_mm(c, l)
                if (mx - x) ** 2 + (my - y) ** 2 <= r2:
                    self.marca(c, l, rede)

    def pinta_retangulo_girado(self, x: float, y: float, larg: float, alt: float,
                               giro: float, rede: int) -> None:
        """Retangulo centrado em (x,y), girado `giro` graus."""
        meia_d = math.hypot(larg, alt) / 2.0
        c0, l0 = self.para_grade(x - meia_d, y - meia_d)
        c1, l1 = self.para_grade(x + meia_d, y + meia_d)
        t = math.radians(-giro)
        ct, st = math.cos(t), math.sin(t)
        hx, hy = larg / 2.0, alt / 2.0
        for l in range(l0, l1 + 1):
            for c in range(c0, c1 + 1):
                mx, my = self.para_mm(c, l)
                dx, dy = mx - x, my - y
                lx = dx * ct - dy * st
                ly = dx * st + dy * ct
                if abs(lx) <= hx and abs(ly) <= hy:
                    self.marca(c, l, rede)

    def pinta_capsula(self, x: float, y: float, larg: float, alt: float,
                      giro: float, rede: int) -> None:
        """Oval do KiCad: retangulo com as duas pontas arredondadas."""
        if larg >= alt:
            raio = alt / 2.0
            meio = (larg - alt) / 2.0
        else:
            raio = larg / 2.0
            meio = (alt - larg) / 2.0
        t = math.radians(giro)
        if larg >= alt:
            dx, dy = math.cos(t) * meio, math.sin(t) * meio
        else:
            dx, dy = -math.sin(t) * meio, math.cos(t) * meio
        self.pinta_circulo(x - dx, y - dy, raio, rede)
        self.pinta_circulo(x + dx, y + dy, raio, rede)
        if larg >= alt:
            self.pinta_retangulo_girado(x, y, larg - alt, alt, giro, rede)
        else:
            self.pinta_retangulo_girado(x, y, larg, alt - larg, giro, rede)

    def pinta_segmento(self, xa: float, ya: float, xb: float, yb: float,
                       raio: float, rede: int) -> None:
        """Trilha: uma capsula de ponta a ponta."""
        passos = max(1, int(math.hypot(xb - xa, yb - ya) / (self.passo / 2.0)))
        for i in range(passos + 1):
            t = i / passos
            self.pinta_circulo(xa + (xb - xa) * t, ya + (yb - ya) * t, raio, rede)

    def pinta_poligono(self, pontos, rede: int, folga: float = 0.0) -> None:
        """Pinta o interior de um poligono (area proibida)."""
        if len(pontos) < 3:
            return
        xs = [p[0] for p in pontos]
        ys = [p[1] for p in pontos]
        c0, l0 = self.para_grade(min(xs) - folga, min(ys) - folga)
        c1, l1 = self.para_grade(max(xs) + folga, max(ys) + folga)
        for l in range(l0, l1 + 1):
            for c in range(c0, c1 + 1):
                mx, my = self.para_mm(c, l)
                # dentro, ou perto o bastante da borda para o cobre invadir
                if (_dentro(mx, my, pontos)
                        or _distancia_ate_borda(mx, my, pontos) <= folga):
                    self.marca(c, l, rede)

    def pinta_fora_do_poligono(self, pontos, rede: int, recuo: float = 0.0) -> None:
        """Pinta tudo que esta FORA do poligono - usado para o contorno da placa."""
        if len(pontos) < 3:
            return
        for l in range(self.linhas):
            for c in range(self.colunas):
                mx, my = self.para_mm(c, l)
                # fora da placa, ou dentro mas perto demais da borda
                if (not _dentro(mx, my, pontos)
                        or _distancia_ate_borda(mx, my, pontos) <= recuo):
                    self.marca(c, l, rede)

    def conta(self):
        livres = sum(1 for v in self.celulas if v == LIVRE)
        ninguem = sum(1 for v in self.celulas if v == DE_NINGUEM)
        return {"total": len(self.celulas), "livres": livres,
                "de_ninguem": ninguem, "de_alguma_rede": len(self.celulas) - livres - ninguem}


def _dentro(x: float, y: float, pontos) -> bool:
    """Ponto dentro do poligono, por contagem de cruzamentos."""
    dentro = False
    n = len(pontos)
    for i in range(n):
        x1, y1 = pontos[i]
        x2, y2 = pontos[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            corte = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < corte:
                dentro = not dentro
    return dentro


# A folga e sempre tratada como DISTANCIA ATE A BORDA, nunca inflando o poligono.
# Inflar poligono concavo na mao erra nos cantos reentrantes, e o erro apareceria
# como trilha encostando numa area proibida - visivel so depois de fresar.


def _distancia_ate_borda(x: float, y: float, pontos) -> float:
    melhor = float("inf")
    n = len(pontos)
    for i in range(n):
        x1, y1 = pontos[i]
        x2, y2 = pontos[(i + 1) % n]
        melhor = min(melhor, _distancia_ao_segmento(x, y, x1, y1, x2, y2))
    return melhor


def _distancia_ao_segmento(px, py, x1, y1, x2, y2) -> float:
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))
