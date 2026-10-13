# -*- coding: utf-8 -*-
"""A* sobre a grade, nas 8 direcoes, com via e com a chegada obrigatoria por baixo.

Por que 8 direcoes: numa grade fina, a diagonal E o segmento a 45 graus. Nao
precisa de geometria continua nem de pos-processamento para "arredondar" o
caminho - o 45 sai da propria vizinhanca. Foi a conclusao que me fez descartar A*
continuo, que seria bem mais pesado e sem ganho.

O estado carrega a DIRECAO DE CHEGADA, nao so a celula. Sem isso nao da para
cobrar por curva, e trilha fresada cheia de degrau fica feia e frageis - o canto
vivo e onde a fresa deixa rebarba e onde o cobre descola.
"""
from __future__ import annotations

import heapq
import math

from .grade import BAIXO, CIMA, DE_NINGUEM

# 8 vizinhos: as 4 retas e as 4 diagonais, nesta ordem para o desempate ser estavel
DIRECOES = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
PARADO = -1

RAIZ2 = math.sqrt(2.0)


class Custos:
    """Quanto custa cada coisa, em unidades de celula.

    Nao ha ponto otimo: curva e via sao substitutos. Encarecer a curva empurra o
    roteador para as vias e vice-versa. O que da para escolher e onde ficar nessa
    troca, e aqui a escolha e por POUCA CURVA: a via nesta bancada e um furo com a
    mesma broca das ilhas, rapida de fazer, enquanto cada degrau na trilha e um
    lugar a mais para a fresa deixar rebarba.
    """

    def __init__(self, curva=6.0, via=40.0, face_de_cima=1.15):
        self.curva = curva
        self.via = via
        # Andar por cima custa um pouco mais: a face de cima e o caminho de
        # passagem, e deixa-la livre ajuda as redes que ainda vao passar por ali.
        self.face_de_cima = face_de_cima


class Roteador:
    def __init__(self, mapa, custos: Custos = None):
        self.mapa = mapa
        self.custos = custos or Custos()
        self._via_cabe = {}

    def _cabe_via(self, c, l, rede) -> bool:
        """A ilha inteira da via cabe aqui, nas duas faces?

        A trilha e fina e o A* a trata celula a celula, mas a VIA e um disco de
        2,5 mm. Tratar a via como um ponto - que foi o que a primeira versao fez -
        poe duas vias de redes diferentes a 2,1 mm uma da outra, com ilha de 2,5:
        curto. Aqui a regra e explicita: so entra via onde todo o disco da ilha,
        mais a folga, estiver livre para esta rede.
        """
        chave = (c, l, rede)
        guardado = self._via_cabe.get(chave)
        if guardado is not None:
            return guardado

        regras = self.mapa.regras
        raio = regras.via_ilha / 2.0 + regras.folga
        passos = int(math.ceil(raio / regras.grade))
        ok = True
        for face in self.mapa.faces.values():
            for dl in range(-passos, passos + 1):
                for dc in range(-passos, passos + 1):
                    if (dc * dc + dl * dl) * regras.grade ** 2 > raio * raio:
                        continue
                    if not face.livre_para(c + dc, l + dl, rede):
                        ok = False
                        break
                if not ok:
                    break
            if not ok:
                break
        self._via_cabe[chave] = ok
        return ok

    # ------------------------------------------------------------------
    def _vizinhos(self, estado, rede):
        c, l, face, veio = estado
        saida = []
        g = self.mapa.faces[face]
        peso_face = self.custos.face_de_cima if face == CIMA else 1.0

        for d, (dc, dl) in enumerate(DIRECOES):
            nc, nl = c + dc, l + dl
            if not g.livre_para(nc, nl, rede):
                continue
            passo = RAIZ2 if dc and dl else 1.0
            custo = passo * peso_face
            if veio != PARADO and veio != d:
                # 45 graus custa menos que 90: a trilha segue mais lisa
                giro = min((d - veio) % 8, (veio - d) % 8)
                custo += self.custos.curva * (1 if giro == 1 else 2)
            saida.append(((nc, nl, face, d), custo))

        # via: troca de face no mesmo lugar. Nao basta a celula estar livre - a
        # ilha inteira tem de caber, nas duas faces.
        outra = BAIXO if face == CIMA else CIMA
        if self._cabe_via(c, l, rede):
            saida.append(((c, l, outra, PARADO), self.custos.via))
        return saida

    # ------------------------------------------------------------------
    def liga(self, rede, origens, destinos):
        """Caminho mais barato de qualquer origem ate qualquer destino.

        Multi-origem e multi-destino de proposito: uma rede com varios pads vai
        sendo ligada em arvore, e o proximo trecho pode sair de QUALQUER ponto do
        que ja esta ligado - inclusive do meio de uma trilha, nao so dos pads.
        """
        alvos = set(destinos)
        if not alvos or not origens:
            return None

        fila = []
        melhor = {}
        for (c, l, face) in origens:
            est = (c, l, face, PARADO)
            melhor[est] = 0.0
            heapq.heappush(fila, (self._estimativa(c, l, alvos), 0.0, est, None))

        veio_de = {}
        while fila:
            _f, g, est, pai = heapq.heappop(fila)
            if est in veio_de:
                continue
            veio_de[est] = pai
            if (est[0], est[1], est[2]) in alvos:
                return self._reconstroi(est, veio_de)
            if g > melhor.get(est, float("inf")):
                continue
            for prox, custo in self._vizinhos(est, rede):
                ng = g + custo
                if ng < melhor.get(prox, float("inf")) - 1e-9:
                    melhor[prox] = ng
                    heapq.heappush(
                        fila, (ng + self._estimativa(prox[0], prox[1], alvos), ng, prox, est))
        return None

    def _estimativa(self, c, l, alvos):
        """Distancia otimista ate o alvo mais perto, na metrica das 8 direcoes."""
        melhor = float("inf")
        for (ac, al, _face) in alvos:
            dc, dl = abs(ac - c), abs(al - l)
            # andando em diagonal enquanto der, depois reto
            d = RAIZ2 * min(dc, dl) + abs(dc - dl)
            if d < melhor:
                melhor = d
        return melhor

    @staticmethod
    def _reconstroi(fim, veio_de):
        caminho = []
        cur = fim
        while cur is not None:
            caminho.append(cur)
            cur = veio_de.get(cur)
        caminho.reverse()
        return caminho


def em_segmentos(caminho, mapa):
    """Transforma o caminho celula a celula em trechos retos e vias.

    O A* devolve um passo por celula; a placa quer o minimo de segmentos. Juntar os
    passos colineares aqui e o que faz a trilha sair lisa em vez de uma escada de
    mil pedacinhos de 0,1 mm.
    """
    if not caminho:
        return [], []
    segmentos, vias = [], []
    inicio = caminho[0]
    anterior = caminho[0]
    for atual in caminho[1:]:
        trocou_de_face = atual[2] != anterior[2]
        mudou_direcao = atual[3] != anterior[3] and atual[3] != PARADO
        if trocou_de_face:
            if (inicio[0], inicio[1]) != (anterior[0], anterior[1]):
                segmentos.append((inicio, anterior))
            vias.append(anterior)
            inicio = atual
        elif mudou_direcao:
            # Guarda so se andou de verdade. O primeiro passo sempre "muda de
            # direcao" (o inicio nao tem direcao nenhuma), e sem esta checagem ele
            # gerava um segmento de comprimento zero - eram 21 dos 69 na primeira
            # placa inteira, lixo que iria parar no .kicad_pcb.
            if (inicio[0], inicio[1]) != (anterior[0], anterior[1]):
                segmentos.append((inicio, anterior))
            inicio = anterior
        anterior = atual
    if (inicio[0], inicio[1]) != (anterior[0], anterior[1]):
        segmentos.append((inicio, anterior))
    return segmentos, vias
