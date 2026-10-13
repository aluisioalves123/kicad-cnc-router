# -*- coding: utf-8 -*-
"""As regras de fabricacao desta bancada, num lugar so.

Os valores nao sao preferencia: sairam da placa `blink blink` v4, que foi fresada e
funcionou. Mexer aqui muda o que o roteador considera possivel, entao cada numero
vem com a razao de ser o que e.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Regras:
    """Medidas de fabricacao, em milimetros."""

    # Largura da trilha. A v4 foi desenhada com 1,0 e saiu limpa; os testes
    # seguintes mostraram que 0,8 tambem sai limpa, e 0,8 e o que permite a
    # densidade maior - que e a razao de ter trocado a perfboard pela fresa.
    trilha: float = 0.8

    # Afastamento entre cobres de redes diferentes. E o clearance que a v4 usou e
    # que a fresa respeitou. Nao confundir com a largura do sulco da V-bit: a
    # fresa abre bem menos que isto, e a folga existe para o cobre nao encostar
    # depois da corrosao do canto e para a solda nao fazer ponte.
    folga: float = 0.4

    # Via. Na bancada ela e um furo comum, feito com a MESMA broca das ilhas - com
    # ilha grande, uma broca unica para tudo e mais comodo que trocar de ferramenta.
    # Entao o furo da via nao e um parametro independente: ele segue a broca.
    broca: float = 0.4
    via_ilha: float = 1.4          # cobre em volta do furo da via

    # Grade do roteador. Aqui ela nao e imposta pela placa, como era na perfboard:
    # e escolha nossa. A 0,1 mm o erro geometrico fica em 0,05 mm, uma ordem de
    # grandeza abaixo do que a fresa resolve, e uma placa de 30x30 da 300x300 nos
    # por camada - A* nisso e instantaneo.
    grade: float = 0.1

    def dilatacao(self) -> float:
        """Quanto inflar cada obstaculo para o centro da trilha poder passar.

        Meia trilha para o cobre nao invadir o obstaculo, mais a folga inteira.
        """
        return self.trilha / 2.0 + self.folga

    def vao_necessario(self) -> float:
        """Vao livre minimo entre dois cobres para uma trilha passar no meio."""
        return self.trilha + 2 * self.folga

    def em_grade(self, mm: float) -> int:
        """Converte milimetros para numero de celulas, arredondando para cima.

        Para cima de proposito: em regra de afastamento, errar para o lado folgado
        gera placa feia; errar para o lado apertado gera placa que nao funciona.
        """
        return int(-(-mm / self.grade // 1))
