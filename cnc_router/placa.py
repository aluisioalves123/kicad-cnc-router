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


# CONVENCAO DE GIRO DO KICAD: o eixo Y aponta para baixo, entao giro positivo na
# tela e NEGATIVO na matematica. mundo = Rot(-giro) . local, e local = Rot(+giro) .
# mundo. Errei este sinal na ida e na volta, e o teste de "reconstrucao exata" nao
# viu porque so testava o angulo que a peca ja tinha - ida e volta com a mesma
# formula errada sempre fecha. Apareceu quando o posicionador escolheu angulo novo
# e as pecas foram parar empilhadas.


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
        t = ang + math.radians(self.giro)       # mundo -> local: +giro
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
    # O KiCad proibe footprint separadamente. Penalizar peca em toda area de regra
    # erraria nas areas que barram so trilha ou so via.
    sem_footprint: bool = False
    # Area que mora DENTRO de um footprint (a "Protection" de uma presilha, por
    # exemplo) anda com ele: guarda a peca dona e o poligono no referencial dela.
    dono: str = ""
    local: list = field(default_factory=list)


@dataclass
class Peca:
    """Um footprint: o que o posicionador precisa mover, girar ou deixar quieto."""

    ref: str
    x: float
    y: float
    giro: float
    travada: bool            # cadeado do proprio KiCad, nao lista nossa
    # Caixa do corpo no referencial DA PECA (giro zero), e o centro dela em
    # relacao a origem. As duas coisas foram erro:
    # - o corpo NAO fica centrado na origem do footprint: num resistor ele esta
    #   3,81 mm deslocado, num TO-92 2,57 mm. Tratar como centrado fez o
    #   posicionador jogar Q2 para fora da placa e empilhar pecas, achando que
    #   estava tudo certo;
    # - a caixa que o KiCad devolve ja vem GIRADA; guardar assim e girar de novo
    #   no posicionador trocava largura e altura duas vezes na peca a 90 graus.
    largura: float
    altura: float
    # Pontos do courtyard no referencial da peca. O corpo de verdade: o C usa o
    # casco convexo deles. largura/altura/cx/cy ficam como reserva para peca
    # sem courtyard.
    corpo: list = field(default_factory=list)
    # Conector: o posicionador leva para a periferia (ver _eh_conector).
    conector: bool = False
    cx: float = 0.0
    cy: float = 0.0
    pads: list = field(default_factory=list)


@dataclass
class Ligacao:
    """Um par de pads da mesma rede que ainda precisa ser ligado."""

    rede: int
    rede_nome: str
    de: str
    ate: str


@dataclass
class Plano:
    """Zona de cobre com rede: o plano (de GND, quase sempre).

    Na fresa e o cobre que sobra depois da isolacao, ligado a uma rede. Pad dessa
    rede encostado no plano ja esta ligado - o roteador so costura os pedacos que
    as outras trilhas separam."""

    rede: int
    rede_nome: str
    camada: str
    poligono: list
    # Plano em cima ligado a pad pelo alivio termico = solda do lado dos
    # componentes, o que o projeto proibe. So conta se a ligacao com pad estiver
    # desligada (ai ele so liga por via).
    liga_pads: bool = True


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
    planos: list = field(default_factory=list)
    trilhas: list = field(default_factory=list)
    vias: list = field(default_factory=list)
    abertas: int = 0
    borda: float = 0.5         # folga cobre-borda do Board Setup
    folga: float = 0.0         # maior folga entre redes que o projeto exige

    def pads_da_rede(self, rede: int):
        return [p for p in self.pads if p.rede == rede]


FORMAS = {
    pcbnew.PAD_SHAPE_CIRCLE: "circulo",
    pcbnew.PAD_SHAPE_OVAL: "oval",
    pcbnew.PAD_SHAPE_RECT: "retangulo",
}


def _caixa_do_courtyard(fp):
    """Caixa do COURTYARD da peca - a regra do KiCad para "aqui nao cabe outra".

    A caixa do footprint (GetBoundingBox) inclui o desenho de fabricacao e e maior
    que o courtyard. Usando ela, a placa feita a mao parecia ter 1,6 mm2 de
    sobreposicao, e eu tolerei 1 mm2 por par para nao reprovar o usuario - o que
    deixou passar um courtyard cruzado que o DRC do KiCad pegou. O DRC mostra zero
    courtyard sobreposto na placa feita a mao: essa e a regua, com tolerancia zero.

    Courtyard redondo (eletrolitico) vira a caixa dele: mais severo que o circulo,
    nunca mais frouxo.
    """
    for camada in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = fp.GetCourtyard(camada)
        if poly.OutlineCount():
            return poly.BBox()
    return fp.GetBoundingBox(False, False)


# Como a perfboard reconhecia: referencia de conector (J1, P2, CN3, X1) ou
# footprint que se declara conector. Pelo nome do footprint pega tambem o
# conector que alguem numerou fora do padrao.
_PREFIXOS_CONECTOR = ("J", "P", "CN", "X")
_NOMES_CONECTOR = ("conn", "pinheader", "pin_header", "pinsocket", "pin_socket",
                   "terminalblock", "terminal_block", "jst", "molex", "usb",
                   "barrel", "jack", "header")


def _eh_conector(fp) -> bool:
    ref = fp.GetReference().upper()
    letras = ref.rstrip("0123456789")
    if letras in _PREFIXOS_CONECTOR and letras != ref:
        return True
    fpid = fp.GetFPID()
    nome = (str(fpid.GetLibNickname()) + ":" + str(fpid.GetLibItemName())).lower()
    return any(n in nome for n in _NOMES_CONECTOR)


def _pontos_do_courtyard(fp) -> list:
    """Vertices do courtyard, no mundo, em mm.

    Os pontos e nao o BBox(): o BBox() do KiCad soma a largura da linha do
    courtyard, e foi isso que fez R3 e R4 - 0,01 mm de vao, segundo o DRC do
    proprio KiCad - parecerem sobrepostos.
    """
    for camada in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = fp.GetCourtyard(camada)
        if poly.OutlineCount():
            pts = []
            for k in range(poly.OutlineCount()):
                o = poly.Outline(k)
                pts += [(mm(o.CPoint(i).x), mm(o.CPoint(i).y))
                        for i in range(o.PointCount())]
            return pts
    return []


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
        caixa_fp = _caixa_do_courtyard(fp)
        ox, oy = mm(fp.GetPosition().x), mm(fp.GetPosition().y)
        giro = fp.GetOrientationDegrees()
        # da caixa do mundo para o referencial da peca
        a = math.radians(giro)                   # mundo -> local: +giro
        wx = mm(caixa_fp.GetCenter().x) - ox
        wy = mm(caixa_fp.GetCenter().y) - oy
        larg, alt = mm(caixa_fp.GetWidth()), mm(caixa_fp.GetHeight())
        if int(round(giro)) % 180 in (90, -90):
            larg, alt = alt, larg
        peca = Peca(
            ref=ref, x=ox, y=oy, giro=giro,
            # Travar peca NAO e lista nossa: e o cadeado que o KiCad ja tem. Quem
            # monta trava na interface que ja conhece, e nada precisa ser mantido
            # em sincronia com o nosso arquivo.
            travada=bool(fp.IsLocked()),
            largura=larg, altura=alt,
            cx=wx * math.cos(a) - wy * math.sin(a),
            cy=wx * math.sin(a) + wy * math.cos(a),
            corpo=[((x - ox) * math.cos(a) - (y - oy) * math.sin(a),
                    (x - ox) * math.sin(a) + (y - oy) * math.cos(a))
                   for x, y in _pontos_do_courtyard(fp)],
            conector=_eh_conector(fp),
        )
        p.pecas.append(peca)
        for pad in fp.Pads():
            peca.pads.append("%s.%s" % (ref, pad.GetNumber()))
            tam = pad.GetSize()
            # offset local: desfaz o giro da peca para guardar o pad no
            # referencial dela
            ang = math.radians(fp.GetOrientationDegrees())   # mundo -> local
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

    # Areas de regra da placa E as que vem dentro dos footprints. Faltavam as do
    # footprint: a "Protection" das presilhas da blink blink ficou de fora, e o
    # DRC do KiCad pegou trilha de GND passando dentro dela.
    zonas = [(zona, None) for zona in board.Zones()]
    for fp in board.GetFootprints():
        zonas += [(zona, fp) for zona in fp.Zones()]
    for zona, fp in zonas:
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
            pontos = [(mm(c.CPoint(k).x), mm(c.CPoint(k).y))
                      for k in range(c.PointCount())]
            z = Proibida(
                camadas=camadas,
                sem_trilha=bool(zona.GetDoNotAllowTracks()),
                sem_via=bool(zona.GetDoNotAllowVias()),
                sem_cobre=bool(zona.GetDoNotAllowZoneFills()),
                sem_footprint=bool(zona.GetDoNotAllowFootprints()),
                poligono=pontos,
            )
            if fp is not None:
                ox, oy = mm(fp.GetPosition().x), mm(fp.GetPosition().y)
                a = math.radians(fp.GetOrientationDegrees())     # mundo -> local
                z.dono = fp.GetReference()
                z.local = [((x - ox) * math.cos(a) - (y - oy) * math.sin(a),
                            (x - ox) * math.sin(a) + (y - oy) * math.cos(a))
                           for x, y in pontos]
            p.proibidas.append(z)

    for zona in board.Zones():
        if zona.GetIsRuleArea() or zona.GetNetCode() <= 0:
            continue
        forma = zona.Outline()
        for camada in zona.GetLayerSet().CuStack():
            for i in range(forma.OutlineCount()):
                c = forma.Outline(i)
                p.planos.append(Plano(
                    rede=zona.GetNetCode(), rede_nome=zona.GetNetname(),
                    camada=board.GetLayerName(camada),
                    poligono=[(mm(c.CPoint(k).x), mm(c.CPoint(k).y))
                              for k in range(c.PointCount())],
                    liga_pads=zona.GetPadConnection() != pcbnew.ZONE_CONNECTION_NONE,
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
    p.borda = mm(board.GetDesignSettings().m_CopperEdgeClearance)
    # A folga EFETIVA de cada pad, ja resolvida pelo KiCad: classe de rede,
    # regra customizada, o que houver. Ler a classe Default na mao deixaria de
    # fora qualquer outra regra que o projeto tenha.
    p.folga = max([mm(pad.GetOwnClearance(pcbnew.B_Cu))
                   for fp in board.GetFootprints() for pad in fp.Pads()
                   if pad.GetNetCode() > 0] or [0.0])
    return p
