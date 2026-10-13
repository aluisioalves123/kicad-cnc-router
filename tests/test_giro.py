# -*- coding: utf-8 -*-
"""O nosso giro tem de bater com o do KiCad em ANGULO NOVO.

O primeiro teste de giro deste projeto era ida e volta: extraia o pad no
referencial da peca e reconstruia no mesmo angulo. Deu erro 0,000000 - e o sinal
estava trocado. Ida e volta com a mesma formula errada sempre fecha; o defeito so
aparece quando a peca vai para um angulo que ela nao tinha, e ai as pecas foram
parar empilhadas e fora da placa.

Aqui o KiCad gira de verdade (SetOrientationDegrees) e a gente compara com o que
a nossa formula previu. 45 graus esta na lista de proposito: em 0/90/180/270 oval
e retangulo sao simetricos e escondem o sinal da forma do pad.

Rodar com o python do KiCad:
    "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe" -m unittest tests.test_giro
"""
import ctypes
import math
import os
import sys
import unittest

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, RAIZ)

import pcbnew  # noqa: E402

from cnc_router import nativo  # noqa: E402
from cnc_router import placa as leitor  # noqa: E402
from cnc_router.regras import Regras  # noqa: E402

PLACA = os.environ.get(
    "PLACA_DE_TESTE",
    r"C:\Users\Aluisio\Desktop\Rino Eletronica\Projetos\blink blink\blink blink.kicad_pcb")
ANGULOS = (0.0, 90.0, 180.0, 270.0, 45.0)
F = pcbnew.ToMM


def local_para_mundo(x, y, giro, dx, dy):
    """A mesma conta que motor.py e posicionador.c fazem: Rot(-giro)."""
    a = math.radians(-giro)
    return (x + dx * math.cos(a) - dy * math.sin(a),
            y + dx * math.sin(a) + dy * math.cos(a))


@unittest.skipUnless(os.path.exists(PLACA), "placa de teste ausente")
class TestGiro(unittest.TestCase):

    def setUp(self):
        self.board = pcbnew.LoadBoard(PLACA)
        self.p = leitor.ler(self.board)

    def test_pad_cai_onde_o_kicad_poe(self):
        pads = {pad.ref: pad for pad in self.p.pads}
        pior = (0.0, "")
        for fp in self.board.GetFootprints():
            ox, oy = F(fp.GetPosition().x), F(fp.GetPosition().y)
            for ang in ANGULOS:
                fp.SetOrientationDegrees(ang)
                for kpad in fp.Pads():
                    nosso = pads["%s.%s" % (fp.GetReference(), kpad.GetNumber())]
                    px, py = local_para_mundo(ox, oy, ang, nosso.dx, nosso.dy)
                    erro = math.hypot(px - F(kpad.GetPosition().x),
                                      py - F(kpad.GetPosition().y))
                    if erro > pior[0]:
                        pior = (erro, "%s a %.0f graus" % (nosso.ref, ang))
        self.assertLess(pior[0], 1e-3, "pad fora do lugar: %.3f mm em %s" % pior)

    def test_corpo_cai_onde_o_kicad_poe(self):
        pecas = {x.ref: x for x in self.p.pecas}
        pior = (0.0, "")
        for fp in self.board.GetFootprints():
            ox, oy = F(fp.GetPosition().x), F(fp.GetPosition().y)
            p = pecas[fp.GetReference()]
            for ang in (0.0, 90.0, 180.0, 270.0):      # caixa so vale em angulo reto
                fp.SetOrientationDegrees(ang)
                bb = leitor._caixa_do_courtyard(fp)
                cx, cy = local_para_mundo(ox, oy, ang, p.cx, p.cy)
                larg, alt = ((p.altura, p.largura) if int(ang) % 180 == 90
                             else (p.largura, p.altura))
                erro = max(abs(cx - F(bb.GetCenter().x)), abs(cy - F(bb.GetCenter().y)),
                           abs(larg - F(bb.GetWidth())), abs(alt - F(bb.GetHeight())))
                if erro > pior[0]:
                    pior = (erro, "%s a %.0f graus" % (p.ref, ang))
        self.assertLess(pior[0], 0.05, "corpo fora do lugar: %.3f mm em %s" % pior)

    def test_courtyard_cai_onde_o_kicad_poe(self):
        """Os pontos do corpo, girados pela nossa conta, contra o courtyard que o
        proprio KiCad gira - inclusive a 45 graus, onde caixa nenhuma serve."""
        pecas = {x.ref: x for x in self.p.pecas}
        pior = (0.0, "")
        for fp in self.board.GetFootprints():
            ox, oy = F(fp.GetPosition().x), F(fp.GetPosition().y)
            p = pecas[fp.GetReference()]
            for ang in ANGULOS:
                fp.SetOrientationDegrees(ang)
                kicad = leitor._pontos_do_courtyard(fp)
                nosso = [local_para_mundo(ox, oy, ang, x, y) for x, y in p.corpo]
                self.assertEqual(len(kicad), len(nosso), p.ref)
                for (kx, ky), (nx, ny) in zip(kicad, nosso):
                    erro = math.hypot(kx - nx, ky - ny)
                    if erro > pior[0]:
                        pior = (erro, "%s a %.0f graus" % (p.ref, ang))
        self.assertLess(pior[0], 1e-3, "courtyard fora do lugar: %.3f mm em %s" % pior)

    def test_forma_do_pad_pintada_bate_com_o_kicad(self):
        """O C pinta o oval girado onde o KiCad diz que o pad esta?

        Sem dilatacao: a forma pura, celula a celula, contra o HitTest do proprio
        KiCad. Erro de uma celula na borda e arredondamento; mais que isso e sinal
        de giro trocado na forma.
        """
        regras = Regras()
        fp = [f for f in self.board.GetFootprints()
              if any(pd.GetShape() == pcbnew.PAD_SHAPE_OVAL for pd in f.Pads())][0]
        fp.SetOrientationDegrees(45.0)
        kpad = [pd for pd in fp.Pads() if pd.GetShape() == pcbnew.PAD_SHAPE_OVAL][0]
        x, y = F(kpad.GetPosition().x), F(kpad.GetPosition().y)
        larg, alt = F(kpad.GetSize().x), F(kpad.GetSize().y)

        rc = nativo.regras_para_c(regras)
        m = nativo.Mapa(rc, x - 4, y - 4, 8, 8)
        sem_dilatacao = -(regras.trilha / 2.0 + regras.folga)
        m.pinta([nativo.PbForma(forma=nativo.CAPSULA, rede=7, faces=nativo.BAIXO,
                                x=x, y=y, larg=larg, alt=alt,
                                giro=kpad.GetOrientationDegrees(),
                                folga_extra=sem_dilatacao)])
        diverge = total = 0
        for l in range(m.linhas):
            for c in range(m.colunas):
                mx, my = m.para_mm(c, l)
                kicad = kpad.HitTest(pcbnew.VECTOR2I(pcbnew.FromMM(mx), pcbnew.FromMM(my)))
                nosso = m.dono(c, l, 1) == 7
                total += kicad
                if kicad != nosso:
                    diverge += 1
        # so a borda pode divergir: menos de 15% da area do pad
        self.assertGreater(total, 0)
        self.assertLess(diverge / total, 0.15,
                        "forma do pad a 45 graus diverge do KiCad em %d de %d celulas"
                        % (diverge, total))


if __name__ == "__main__":
    unittest.main()
