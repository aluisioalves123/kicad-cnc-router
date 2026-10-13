# -*- coding: utf-8 -*-
"""A janela do plugin dentro do pcbnew.

Por enquanto ela so mostra o que o plugin enxerga da placa. E de proposito: antes
de mexer em cobre nenhum, quem usa precisa poder conferir que o plugin leu a placa
certa - contorno, areas proibidas, pecas travadas e o que falta ligar. Se a leitura
estiver errada, o roteamento estaria errado junto e seria bem mais dificil de ver.
"""
from __future__ import annotations

import wx

from .cnc_router import placa as leitor
from .cnc_router.regras import Regras


class Janela(wx.Frame):
    def __init__(self, board):
        super().__init__(None, title="Roteador CNC", size=wx.Size(620, 520))
        self.board = board
        self.regras = Regras()

        painel = wx.Panel(self)
        caixa = wx.BoxSizer(wx.VERTICAL)

        self.texto = wx.TextCtrl(
            painel, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_DONTWRAP)
        self.texto.SetFont(wx.Font(wx.FontInfo(9).Family(wx.FONTFAMILY_TELETYPE)))
        caixa.Add(self.texto, 1, wx.EXPAND | wx.ALL, 8)

        linha = wx.BoxSizer(wx.HORIZONTAL)
        b_ler = wx.Button(painel, label="Reler a placa")
        b_ler.Bind(wx.EVT_BUTTON, self.ao_reler)
        linha.Add(b_ler, 0, wx.RIGHT, 6)
        b_fechar = wx.Button(painel, label="Fechar")
        b_fechar.Bind(wx.EVT_BUTTON, lambda _e: self.Close())
        linha.Add(b_fechar, 0)
        caixa.Add(linha, 0, wx.ALL, 8)

        painel.SetSizer(caixa)
        self.ao_reler(None)

    def ao_reler(self, _evento):
        self.texto.SetValue(self.resumo())

    def resumo(self) -> str:
        r = self.regras
        p = leitor.ler(self.board)
        L = []

        L.append("REGRAS DE FABRICACAO")
        L.append("  trilha %.2f mm | folga %.2f mm | broca %.2f mm"
                 % (r.trilha, r.folga, r.broca))
        L.append("  para passar entre dois cobres sao precisos %.2f mm"
                 % r.vao_necessario())
        L.append("  grade do roteador: %.2f mm" % r.grade)
        L.append("")

        L.append("PLACA")
        L.append("  contorno .......... %.1f x %.1f mm" % (p.largura, p.altura))
        L.append("  pecas ............. %d (%d travadas)"
                 % (len(p.pecas), sum(1 for x in p.pecas if x.travada)))
        L.append("  pads .............. %d" % len(p.pads))
        L.append("  trilhas ........... %d" % len(p.trilhas))
        L.append("  vias .............. %d" % len(p.vias))
        L.append("  ligacoes abertas .. %d" % p.abertas)
        L.append("")

        L.append("AREAS PROIBIDAS: %d" % len(p.proibidas))
        for z in p.proibidas:
            barra = ", ".join(n for n, v in (("trilha", z.sem_trilha),
                                             ("via", z.sem_via),
                                             ("cobre", z.sem_cobre)) if v)
            L.append("  %d pontos | %s | barra: %s"
                     % (len(z.poligono), "+".join(z.camadas) or "nenhuma camada",
                        barra or "nada"))
        L.append("")

        travadas = [x.ref for x in p.pecas if x.travada]
        L.append("PECAS TRAVADAS: %s" % (", ".join(travadas) if travadas else
                                         "nenhuma - o cadeado e o do proprio KiCad"))
        L.append("")

        L.append("TRILHA DE CIMA TERMINANDO EM PAD")
        L.append("  (cada uma destas obriga solda do lado dos componentes)")
        encostam = sorted(self._topo_em_pad(p))
        L.append("  %d pad(s): %s" % (len(encostam), ", ".join(encostam) or "nenhum"))
        return "\n".join(L)

    @staticmethod
    def _topo_em_pad(p):
        import math
        achados = set()
        for t in p.trilhas:
            if t[5] != "F.Cu":
                continue
            for px, py in ((t[0], t[1]), (t[2], t[3])):
                for pad in p.pads:
                    if math.hypot(pad.x - px, pad.y - py) < 0.35:
                        achados.add(pad.ref)
        return achados


def abrir(board):
    Janela(board).Show()
