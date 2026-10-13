# -*- coding: utf-8 -*-
"""Le o que importa de um board do KiCad, sem depender do resto da API.

Tudo o que o roteador precisa sai daqui como numero em milimetro: contorno, pads,
redes, areas proibidas e o que ja esta roteado. O resto do programa nao importa
`pcbnew` - assim ele roda e e testado fora do KiCad, e so a traducao vive aqui.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import pcbnew


def mm(valor) -> float:
    return pcbnew.ToMM(valor)


@dataclass
class Pad:
    """Um pad, ja em milimetros e com o que decide roteamento."""

    ref: str                 # R1.2
    rede: int
    rede_nome: str
    x: float
    y: float
    largura: float           # eixo local x
    altura: float            # eixo local y
    giro: float              # graus
    forma: str               # "circulo", "oval", "retangulo", "outro"
    furo: float              # 0 = SMD
    passante: bool
    # Posicao no referencial da PECA, antes do giro dela. Sem isto, mover um
    # footprint deixaria os pads para tras: a posicao absoluta so vale enquanto a
    # peca nao anda.
    dono: str = ""
    dx: float = 0.0
    dy: float = 0.0
    giro_local: float = 0.0

    def raio_na_direcao(self, ang: float) -> float:
        """Meio-eixo do pad na direcao `ang` (radianos, global).

        Existe porque a pergunta que importa nunca e "qual o tamanho do pad", e sim
        "quanto ele avanca NA DIRECAO do vizinho". Num oval de 1,8 x 3 virado para o
        pino ao lado, usar o eixo maior erra o vao em mais de um milimetro - e foi
        esse erro que quase me fez concluir que pads de 2,54 se tocam.
        """
        a = self.largura / 2.0
        b = self.altura / 2.0
        t = ang - math.radians(self.giro)
        ct, st = math.cos(t), math.sin(t)
        if self.forma == "circulo":
            return a
        if self.forma == "oval":
            # elipse: r(t) = 1 / sqrt((cos t / a)^2 + (sin t / b)^2)
            return 1.0 / math.hypot(ct / a, st / b)
        # retangulo e o resto: ate a borda, pelo lado que o raio cruza primeiro
        return min(a / abs(ct) if ct else float("inf"),
                   b / abs(st) if st else float("inf"))


@dataclass
class Proibida:
    """Area de regra do KiCad. Pode valer so para algumas camadas e barrar so
    algumas coisas - ha area que proibe via e deixa passar trilha."""

    camadas: list
    sem_trilha: bool
    sem_via: bool
    sem_cobre: bool
    poligono: list


@dataclass
class Peca:
    """Um footprint: o que o posicionador precisa mover, girar ou deixar quieto."""

    ref: str
    x: float
    y: float
    giro: float
    travada: bool            # cadeado do proprio KiCad, nao lista nossa
    largura: float           # caixa do footprint, em mm
    altura: float
    pads: list = field(default_factory=list)


@dataclass
class Ligacao:
    """Um par de pads da mesma rede que ainda precisa ser ligado."""

    rede: int
    rede_nome: str
    de: str
    ate: str


@dataclass
class Placa:
    nome: str
    contorno: list = field(default_factory=list)   # poligono em mm
    largura: float = 0.0
    altura: float = 0.0
    x0: float = 0.0
    y0: float = 0.0
    pecas: list = field(default_factory=list)
    pads: list = field(default_factory=list)
    proibidas: list = field(default_factory=list)
    trilhas: list = field(default_factory=list)
    vias: list = field(default_factory=list)
    abertas: int = 0

    def pads_da_rede(self, rede: int):
        return [p for p in self.pads if p.rede == rede]


FORMAS = {
    pcbnew.PAD_SHAPE_CIRCLE: "circulo",
    pcbnew.PAD_SHAPE_OVAL: "oval",
    pcbnew.PAD_SHAPE_RECT: "retangulo",
}


def _poligono_do_contorno(board) -> list:
    """Contorno da placa como lista de pontos, em mm.

    Usa o proprio KiCad para fechar o contorno: juntar segmentos de Edge.Cuts na
    mao erra em arco e em canto, e o resultado silencioso seria o roteador achando
    que pode sair da placa.
    """
    forma = pcbnew.SHAPE_POLY_SET()
    # o segundo argumento manda o KiCad deduzir um contorno quando o Edge.Cuts
    # esta aberto ou ausente; sem isso a placa sem contorno devolveria nada e o
    # roteador acharia que pode trabalhar no vazio
    if not board.GetBoardPolygonOutlines(forma, True):
        return []
    if forma.OutlineCount() == 0:
        return []
    contorno = forma.Outline(0)
    return [(mm(contorno.CPoint(i).x), mm(contorno.CPoint(i).y))
            for i in range(contorno.PointCount())]


def ler(board) -> Placa:
    """Traduz um BOARD do pcbnew para a nossa descricao em milimetros."""
    p = Placa(nome=board.GetFileName().split("\\")[-1] or "sem nome")

    p.contorno = _poligono_do_contorno(board)
    caixa = board.GetBoardEdgesBoundingBox()
    p.x0, p.y0 = mm(caixa.GetX()), mm(caixa.GetY())
    p.largura, p.altura = mm(caixa.GetWidth()), mm(caixa.GetHeight())

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        caixa_fp = fp.GetBoundingBox(False, False)
        peca = Peca(
            ref=ref,
            x=mm(fp.GetPosition().x), y=mm(fp.GetPosition().y),
            giro=fp.GetOrientationDegrees(),
            # Travar peca NAO e lista nossa: e o cadeado que o KiCad ja tem. Quem
            # monta trava na interface que ja conhece, e nada precisa ser mantido
            # em sincronia com o nosso arquivo.
            travada=bool(fp.IsLocked()),
            largura=mm(caixa_fp.GetWidth()), altura=mm(caixa_fp.GetHeight()),
        )
        p.pecas.append(peca)
        for pad in fp.Pads():
            peca.pads.append("%s.%s" % (ref, pad.GetNumber()))
            tam = pad.GetSize()
            # offset local: desfaz o giro da peca para guardar o pad no
            # referencial dela
            ang = math.radians(-fp.GetOrientationDegrees())
            ox = mm(pad.GetPosition().x) - peca.x
            oy = mm(pad.GetPosition().y) - peca.y
            lx = ox * math.cos(ang) - oy * math.sin(ang)
            ly = ox * math.sin(ang) + oy * math.cos(ang)
            p.pads.append(Pad(
                ref="%s.%s" % (ref, pad.GetNumber()),
                rede=pad.GetNetCode(),
                rede_nome=pad.GetNetname(),
                x=mm(pad.GetPosition().x), y=mm(pad.GetPosition().y),
                largura=mm(tam.x), altura=mm(tam.y),
                giro=pad.GetOrientationDegrees(),
                forma=FORMAS.get(pad.GetShape(), "outro"),
                furo=mm(pad.GetDrillSizeX()),
                passante=pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH,
                dono=ref, dx=lx, dy=ly,
                giro_local=pad.GetOrientationDegrees() - fp.GetOrientationDegrees(),
            ))

    for zona in board.Zones():
        if not zona.GetIsRuleArea():
            continue
        # Area proibida vale para um CONJUNTO de camadas, nao para uma. Lida com
        # GetLayer() ela devolve "undefined" e a informacao se perde - foi o que
        # aconteceu na primeira versao deste leitor.
        camadas = [board.GetLayerName(l) for l in zona.GetLayerSet().CuStack()]
        # e cada area proibe coisas diferentes: ha area que barra via e deixa
        # passar trilha. Tratar tudo como "proibido" perderia espaco util.
        forma = zona.Outline()
        for i in range(forma.OutlineCount()):
            c = forma.Outline(i)
            p.proibidas.append(Proibida(
                camadas=camadas,
                sem_trilha=bool(zona.GetDoNotAllowTracks()),
                sem_via=bool(zona.GetDoNotAllowVias()),
                sem_cobre=bool(zona.GetDoNotAllowZoneFills()),
                poligono=[(mm(c.CPoint(k).x), mm(c.CPoint(k).y))
                          for k in range(c.PointCount())],
            ))

    for t in board.Tracks():
        if t.GetClass() == "PCB_VIA":
            p.vias.append((mm(t.GetPosition().x), mm(t.GetPosition().y),
                           mm(t.GetWidth()), mm(t.GetDrill()), t.GetNetCode()))
        else:
            p.trilhas.append((mm(t.GetStart().x), mm(t.GetStart().y),
                              mm(t.GetEnd().x), mm(t.GetEnd().y),
                              mm(t.GetWidth()), board.GetLayerName(t.GetLayer()),
                              t.GetNetCode()))

    p.abertas = board.GetConnectivity().GetUnconnectedCount(False)
    return p
