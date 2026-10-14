# -*- coding: utf-8 -*-
"""A janela do plugin dentro do pcbnew.

Mostra o que o plugin enxerga da placa, posiciona e roteia, e so mexe no board
depois de voce confirmar.

Duas decisoes que nao sao de gosto:

- A janela e MODAL. O KiCad compara o board antes e depois do Run() do plugin e
  transforma a diferenca num passo de desfazer. Com janela solta, o Run() voltava
  na hora, a mudanca vinha depois e ficava fora do Ctrl+Z.
- O sorteio roda numa thread, e a thread nunca toca no board: o pcbnew nao e
  seguro fora da thread da interface. Ela trabalha no modelo (placa.py + C), e
  quem le e escreve o board e sempre a thread da janela.
"""
from __future__ import annotations

import threading
import time

import wx

from .cnc_router import escreve as escritor
from .cnc_router import motor, nativo
from .cnc_router import placa as leitor
from .cnc_router.confere import confere
from .cnc_router.regras import Regras


class Janela(wx.Dialog):
    def __init__(self, board):
        super().__init__(None, title="Roteador CNC", size=wx.Size(680, 660),
                         style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)
        self.board = board
        self.regras = Regras()
        self.placar = None
        self.thread = None
        self.inicio = 0.0
        self.prazo = 0.0
        self.resultado = None       # (placa, res, problemas) pronto para aplicar
        self.mudam, self.existentes = [], 0
        self.parando = False
        self.fase = ""
        self.abertas_hoje = 0

        painel = wx.Panel(self)
        caixa = wx.BoxSizer(wx.VERTICAL)

        self.texto = wx.TextCtrl(
            painel, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.TE_DONTWRAP)
        self.texto.SetFont(wx.Font(wx.FontInfo(9).Family(wx.FONTFAMILY_TELETYPE)))
        caixa.Add(self.texto, 1, wx.EXPAND | wx.ALL, 8)

        opcoes = wx.BoxSizer(wx.HORIZONTAL)
        self.c_mover = wx.CheckBox(painel, label="Mover as pecas (as travadas ficam)")
        self.c_mover.SetValue(True)
        opcoes.Add(self.c_mover, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 16)
        self.c_conectores = wx.CheckBox(painel, label="Conectores na periferia")
        self.c_conectores.SetValue(True)
        opcoes.Add(self.c_conectores, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 16)
        self.c_lados = wx.CheckBox(painel, label="Pecas dos dois lados")
        self.c_lados.SetToolTip("Pode virar pecas para baixo. Cada peca continua "
                                "soldando de um lado so: o oposto ao corpo dela.")
        self.c_lados.SetValue(False)
        opcoes.Add(self.c_lados, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 16)
        opcoes.Add(wx.StaticText(painel, label="Tempo maximo:"), 0,
                   wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        self.s_tempo = wx.SpinCtrl(painel, min=5, max=3600, initial=120,
                                   size=wx.Size(80, -1))
        opcoes.Add(self.s_tempo, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)
        opcoes.Add(wx.StaticText(painel, label="s"), 0, wx.ALIGN_CENTER_VERTICAL)
        caixa.Add(opcoes, 0, wx.LEFT | wx.RIGHT, 8)

        self.status = wx.StaticText(painel, label="")
        caixa.Add(self.status, 0, wx.EXPAND | wx.ALL, 8)
        self.barra = wx.Gauge(painel, range=1000)
        caixa.Add(self.barra, 0, wx.EXPAND | wx.LEFT | wx.RIGHT, 8)

        botoes = wx.BoxSizer(wx.HORIZONTAL)
        self.b_ler = wx.Button(painel, label="Reler a placa")
        self.b_rodar = wx.Button(painel, label="Posicionar e rotear")
        self.b_parar = wx.Button(painel, label="Parar")
        self.b_aplicar = wx.Button(painel, label="Aplicar na placa")
        self.b_fechar = wx.Button(painel, label="Fechar")
        for b in (self.b_ler, self.b_rodar, self.b_parar, self.b_aplicar):
            botoes.Add(b, 0, wx.RIGHT, 6)
        botoes.AddStretchSpacer()
        botoes.Add(self.b_fechar, 0)
        caixa.Add(botoes, 0, wx.EXPAND | wx.ALL, 8)

        self.b_ler.Bind(wx.EVT_BUTTON, self.ao_reler)
        self.b_rodar.Bind(wx.EVT_BUTTON, self.ao_rodar)
        self.b_parar.Bind(wx.EVT_BUTTON, self.ao_parar)
        self.b_aplicar.Bind(wx.EVT_BUTTON, self.ao_aplicar)
        self.b_fechar.Bind(wx.EVT_BUTTON, lambda _e: self.Close())
        self.Bind(wx.EVT_CLOSE, self.ao_fechar)

        self.relogio = wx.Timer(self)
        self.Bind(wx.EVT_TIMER, self.ao_relogio, self.relogio)

        painel.SetSizer(caixa)
        self.ao_reler(None)

    # ------------------------------------------------------------ estado

    def _estado(self, rodando):
        self.b_ler.Enable(not rodando)
        self.b_rodar.Enable(not rodando)
        self.c_mover.Enable(not rodando)
        self.c_conectores.Enable(not rodando)
        self.c_lados.Enable(not rodando)
        self.s_tempo.Enable(not rodando)
        self.b_parar.Enable(rodando)
        self.b_aplicar.Enable(not rodando and self.resultado is not None)

    def ao_reler(self, _evento):
        self.resultado = None
        self.texto.SetValue(self.resumo())
        self.status.SetLabel("")
        self.barra.SetValue(0)
        self._estado(rodando=False)

    # ------------------------------------------------------------ rodar

    def ao_rodar(self, _evento):
        # o board e lido AQUI, na thread da janela; a thread so ve o modelo
        p = leitor.ler(self.board)
        self.regras = Regras().do_projeto(p)
        self.abertas_hoje = p.abertas
        p.trilhas, p.vias = [], []          # roteia do zero
        self.resultado = None
        self.placar = nativo.PbPlacar()
        self.inicio = time.time()
        mover = self.c_mover.GetValue()
        self.periferia = self.c_conectores.GetValue()
        self.dois_lados = self.c_lados.GetValue()
        segundos = float(self.s_tempo.GetValue())
        self.prazo = segundos if mover else 0.0
        self.status.SetLabel("preparando...")
        self.parando = False
        self.fase = "sorteio" if mover else "final"
        self._estado(rodando=True)
        self.thread = threading.Thread(target=self._trabalho,
                                       args=(p, mover, segundos), daemon=True)
        self.thread.start()
        self.relogio.Start(300)

    def _trabalho(self, p, mover, segundos):
        try:
            if mover:
                motor.posiciona(p, self.regras, segundos=segundos, placar=self.placar,
                                conectores_na_periferia=self.periferia,
                                dois_lados=self.dois_lados)
            self.fase = "final"             # daqui em diante nao ha o que parar
            res = motor.roteia(p, self.regras)
            problemas = confere(p, self.regras, res)
            wx.CallAfter(self._pronto, p, res, problemas, mover)
        except Exception:
            import traceback
            wx.CallAfter(self._falhou, traceback.format_exc())

    def ao_relogio(self, _evento):
        pl = self.placar
        if pl is None:
            return
        passou = time.time() - self.inicio
        # Parar e fase final tem de aparecer e FICAR: antes o relogio escrevia o
        # progresso por cima do "parando..." 0,3 s depois e a barra seguia
        # andando com o tempo - o botao parecia nao fazer nada.
        if self.fase == "final":
            self.b_parar.Enable(False)
            self.barra.Pulse()
            self.status.SetLabel("roteamento final e conferencia... %.0f s  (%d posicoes "
                                 "roteadas no sorteio)" % (passou, pl.julgados))
        elif self.parando:
            self.barra.Pulse()
            self.status.SetLabel("parando: cada nucleo termina a posicao em que esta "
                                 "(%d posicoes roteadas ate aqui)..." % pl.julgados)
        elif self.prazo > 0:
            self.barra.SetValue(min(1000, int(1000 * passou / self.prazo)))
            self.status.SetLabel(
                "%.0f s de %.0f | posicoes roteadas: %d | melhor ate agora: %d "
                "aberta(s), %d via(s), plano perdido %.0f mm2, %.1f mm de peca na "
                "frente dos conectores  (sem mover pecas: %d)"
                % (passou, self.prazo, pl.julgados, pl.abertas_depois, pl.vias,
                   pl.plano_perdido, pl.conectores_mm, pl.abertas_antes))
        else:
            self.barra.Pulse()
            self.status.SetLabel("roteando... %.0f s" % passou)

    def ao_parar(self, _evento):
        if self.placar is not None and not self.parando:
            self.placar.parar = 1
            self.parando = True
            self.b_parar.Enable(False)
            self.ao_relogio(None)

    def _falhou(self, detalhe):
        self.relogio.Stop()
        self._estado(rodando=False)
        self.status.SetLabel("o plugin quebrou - nada foi mudado na placa")
        wx.MessageBox(detalhe, "Roteador CNC", wx.OK | wx.ICON_ERROR, self)

    def _pronto(self, p, res, problemas, mover):
        self.relogio.Stop()
        self.barra.SetValue(1000)
        self.resultado = (p, res, problemas)
        self.mudam = escritor.pecas_que_mudam(self.board, p) if mover else []
        self.existentes = len(list(self.board.Tracks()))
        L = ["RESULTADO (ainda NAO aplicado na placa)", ""]
        if mover:
            pl = self.placar
            L.append("  sorteio ............ %.0f s, %d posicoes roteadas"
                     % (time.time() - self.inicio, pl.julgados))
            L.append("  ligacoes abertas ... %d (rotear sem mover pecas daria %d)"
                     % (len(res.abertas), pl.abertas_antes))
            if p.planos:
                L.append("  plano perdido ...... %.0f mm2 (trilha alheia + ilhas soltas)"
                         % res.plano_perdido)
            if self.periferia:
                L.append("  conectores ......... %s"
                         % ("na periferia" if pl.conectores_mm == 0 else
                            "%.1f mm de peca na frente" % pl.conectores_mm))
            L.append("  pecas que mudam .... %d: %s"
                     % (len(self.mudam), ", ".join(self.mudam) or "nenhuma"))
        else:
            L.append("  ligacoes abertas ... %d" % len(res.abertas))
        L.append("  a placa hoje ....... %d sem ligacao, pelo KiCad" % self.abertas_hoje)
        L.append("  trilhas novas ...... %d" % len(res.trilhas))
        L.append("  vias novas ......... %d" % len(res.vias))
        L.append("  trilhas/vias atuais  %d (serao apagadas)" % self.existentes)
        if res.abertas:
            L.append("")
            L.append("NAO FECHARAM: %d" % len(res.abertas))
            for rede, pad in res.abertas:
                L.append("  %s: falta chegar em %s" % (rede, pad))
        L.append("")
        L.append("CONFERENCIA (medida em mm, independente do roteador)")
        L += ["  ! " + x for x in problemas] or ["  nenhum problema"]
        L.append("")
        L.append("'Aplicar na placa' grava isto no board; Ctrl+Z no KiCad desfaz.")
        self.texto.SetValue("\n".join(L))
        self.status.SetLabel("pronto: %d ligacao(oes) aberta(s)" % len(res.abertas))
        self._estado(rodando=False)

    # ------------------------------------------------------------ aplicar

    def ao_aplicar(self, _evento):
        if self.resultado is None:
            return
        p, res, problemas = self.resultado
        aviso = ("Isto vai:\n"
                 "  - mover %d peca(s)\n"
                 "  - apagar %d trilha(s)/via(s) que a placa tem hoje\n"
                 "  - criar %d trilha(s) e %d via(s)\n"
                 % (len(self.mudam), self.existentes, len(res.trilhas), len(res.vias)))
        if problemas:
            aviso += "\nATENCAO: a conferencia achou %d problema(s).\n" % len(problemas)
        aviso += "\nDa para desfazer com Ctrl+Z depois. Aplicar?"
        if wx.MessageBox(aviso, "Aplicar na placa", wx.YES_NO | wx.NO_DEFAULT
                         | wx.ICON_QUESTION, self) != wx.YES:
            return
        escritor.move_pecas(self.board, p)
        escritor.limpa(self.board)
        escritor.escreve(self.board, res, self.regras)
        # fecha para o Run() terminar: e ai que o KiCad registra o desfazer e
        # redesenha a placa
        self.EndModal(wx.ID_OK)

    # ------------------------------------------------------------ fechar

    def ao_fechar(self, _evento):
        if self.thread is not None and self.thread.is_alive():
            self.ao_parar(None)
            wx.BeginBusyCursor()
            try:
                self.thread.join()      # o C para em segundos, nao em minutos
            finally:
                wx.EndBusyCursor()
        self.relogio.Stop()
        if self.IsModal():
            self.EndModal(wx.ID_CANCEL)
        else:
            self.Destroy()

    # ------------------------------------------------------------ leitura

    def resumo(self) -> str:
        p = leitor.ler(self.board)
        r = self.regras = Regras().do_projeto(p)
        L = []

        L.append("REGRAS DE FABRICACAO")
        L.append("  trilha %.2f mm | folga %.2f mm | broca %.2f mm | via %.2f mm"
                 % (r.trilha, r.folga, r.broca, r.via_ilha))
        L.append("  cobre ate a borda: %.2f mm (Board Setup)" % p.borda)
        L.append("  a folga e a maior entre 0,40 (fresa) e a do projeto (%.2f)" % p.folga)
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

        L.append("PLANOS: %d" % len(p.planos))
        for z in p.planos:
            if z.camada == "B.Cu" and z.liga_pads:
                uso = "liga os pads - o roteador so costura os pedacos"
            elif z.camada == "B.Cu":
                uso = "nao liga pads (conexao desligada): nao ajuda o roteador"
            elif z.liga_pads:
                uso = ("ATENCAO: liga pads pela face de CIMA = solda do lado dos "
                       "componentes. Desligue a conexao com pads desse plano")
            else:
                uso = "em cima, so por via: o roteador faz trilha normal"
            L.append("  %s no %s | %s" % (z.rede_nome, z.camada, uso))
        L.append("")

        L.append("AREAS PROIBIDAS: %d" % len(p.proibidas))
        for z in p.proibidas:
            barra = ", ".join(n for n, v in (("trilha", z.sem_trilha),
                                             ("via", z.sem_via),
                                             ("cobre", z.sem_cobre),
                                             ("footprint", z.sem_footprint)) if v)
            L.append("  %d pontos | %s | barra: %s"
                     % (len(z.poligono), "+".join(z.camadas) or "nenhuma camada",
                        barra or "nada"))
        L.append("")

        travadas = [x.ref for x in p.pecas if x.travada]
        conectores = [x.ref for x in p.pecas if x.conector]
        L.append("CONECTORES (vao para a periferia): %s"
                 % (", ".join(conectores) or "nenhum reconhecido"))
        L.append("")
        L.append("PECAS TRAVADAS: %s" % (", ".join(travadas) if travadas else
                                         "nenhuma - o cadeado e o do proprio KiCad"))
        L.append("")

        L.append("TRILHA DE CIMA TERMINANDO EM PAD")
        L.append("  (cada uma destas obriga solda do lado dos componentes)")
        encostam = sorted(self._topo_em_pad(p))
        L.append("  %d pad(s): %s" % (len(encostam), ", ".join(encostam) or "nenhum"))
        L.append("")
        L.append("'Posicionar e rotear' calcula sem mexer na placa; voce ve o")
        L.append("resultado e decide se aplica.")
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
    dlg = Janela(board)
    try:
        dlg.ShowModal()
    finally:
        dlg.Destroy()
