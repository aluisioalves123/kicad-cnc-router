# -*- coding: utf-8 -*-
"""Registra o plugin no KiCad.

O KiCad importa este pacote na abertura do pcbnew e chama `register()` em tudo que
herda de `ActionPlugin`. O botao aparece em Ferramentas -> Plugins externos, e
tambem na barra de ferramentas por causa de `show_toolbar_button`.

Importar o roteador aqui dentro, e nao no topo, e de proposito: erro de sintaxe
num modulo nosso derrubaria o carregamento de TODOS os plugins do KiCad, e a
mensagem que aparece nao diz de quem foi a culpa. Assim o botao sempre nasce, e um
erro nosso vira uma janela com o traceback.
"""
import os

import pcbnew


class RoteadorCNC(pcbnew.ActionPlugin):
    def defaults(self):
        self.name = "Roteador CNC"
        self.category = "Roteamento"
        self.description = ("Posiciona e roteia para fresagem caseira: trilha a 45 graus, "
                            "via antes do pad e nada de solda por cima.")
        self.show_toolbar_button = True
        # So aponta o icone se ele existir de verdade: caminho quebrado aqui vira
        # erro na barra de ferramentas do KiCad, e o plugin some sem explicar.
        pasta = os.path.dirname(__file__)
        icone = os.path.join(pasta, "icone.png")
        if os.path.exists(icone):
            self.icon_file_name = icone
        escuro = os.path.join(pasta, "icone_escuro.png")
        if os.path.exists(escuro):
            self.dark_icon_file_name = escuro

    def Run(self):
        try:
            from .interface import abrir
        except Exception:
            import traceback
            _erro("Nao consegui carregar o plugin", traceback.format_exc())
            return
        try:
            abrir(pcbnew.GetBoard())
        except Exception:
            import traceback
            _erro("O plugin quebrou", traceback.format_exc())


def _erro(titulo, detalhe):
    """Mostra o traceback em vez de morrer calado dentro do KiCad."""
    try:
        import wx
        dlg = wx.MessageDialog(None, detalhe, titulo, wx.OK | wx.ICON_ERROR)
        dlg.ShowModal()
        dlg.Destroy()
    except Exception:
        print(titulo + "\n" + detalhe)


# Registrar exige o contexto do programa KiCad: fora dele o wxWidgets dispara um
# assert ("PgmOrNull() failed") e abre uma caixa de erro. Sem esta guarda o modulo
# nao pode nem ser importado para teste - e e justamente importando que a gente
# descobre erro de sintaxe antes de abrir o pcbnew.
def _dentro_do_kicad() -> bool:
    # O sinal nao pode ser "existe tal funcao no pcbnew": o modulo inteiro existe
    # fora do KiCad, e foi assim que a primeira versao desta guarda falhou. O que
    # so existe com o programa aberto e uma aplicacao wx rodando.
    try:
        import wx
        return wx.GetApp() is not None
    except Exception:
        return False


if _dentro_do_kicad():
    RoteadorCNC().register()
