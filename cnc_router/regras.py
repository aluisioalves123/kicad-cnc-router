# -*- coding: utf-8 -*-
"""As regras de fabricacao desta bancada, num lugar so.

Os valores nao sao preferencia: sairam da placa `blink blink` v4, que foi fresada e
funcionou. Mexer aqui muda o que o roteador considera possivel, entao cada numero
vem com a razao de ser o que e.
"""
from __future__ import annotations

from dataclasses import dataclass

# As duas faces de cobre, pelo nome que o KiCad usa.
CIMA = "F.Cu"
BAIXO = "B.Cu"


class Custos:
    """Quanto custa cada coisa para o roteador, em unidades de celula.

    Nao ha ponto otimo: curva e via sao substitutos. Encarecer a curva empurra o
    roteador para as vias e vice-versa. O que da para escolher e onde ficar nessa
    troca, e aqui a escolha e por POUCA CURVA: a via nesta bancada e um furo com a
    mesma broca das ilhas, rapida de fazer, enquanto cada degrau na trilha e um
    lugar a mais para a fresa deixar rebarba.
    """

    def __init__(self, curva=6.0, via=40.0, face_de_cima=1.15, plano=2.0,
                 estimativa=1.0):
        self.curva = curva
        self.via = via
        # Atravessar o plano de outra rede pela face de baixo: cada trilha ali
        # corta o plano de GND. Na PCB o plano vale mais que fio curto.
        self.plano = plano
        # A* ponderado: a estimativa ate o alvo vale isto vezes mais. Em placa
        # grande corta a busca de minutos para segundos; o caminho sai um pouco
        # menos otimo.
        self.estimativa = estimativa
        # Andar por cima custa um pouco mais: a face de cima e o caminho de
        # passagem, e deixa-la livre ajuda as redes que ainda vao passar por ali.
        self.face_de_cima = face_de_cima


CELULAS_MAX = 250000      # por face; acima disso a grade engrossa (ver do_projeto)


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
    # A broca e a MESMA das ilhas dos componentes: com ilha grande, uma ferramenta
    # so para tudo e mais comoda na usinagem do que trocar de broca no meio.
    broca: float = 0.8
    # E a ilha da via tambem e do tamanho da ilha de componente, porque ela e
    # soldada do mesmo jeito - com fio atravessando e ferro dos dois lados. Ilha
    # pequena fica bonita na tela e impossivel de soldar na bancada.
    via_ilha: float = 2.5

    # Grade do roteador. Aqui ela nao e imposta pela placa, como era na perfboard:
    # e escolha nossa. A 0,1 mm o erro geometrico fica em 0,05 mm, uma ordem de
    # grandeza abaixo do que a fresa resolve, e uma placa de 30x30 da 300x300 nos
    # por camada - A* nisso e instantaneo.
    grade: float = 0.1

    # Cobre ate a borda da placa. Vem do projeto (Board Setup do KiCad), nao
    # daqui: o leitor sobrescreve. Faltava, e o DRC do KiCad pegou um pad de R4
    # colado no Edge.Cuts - a trilha tambem ficava a 0,4 mm da borda, abaixo
    # dos 0,5 que a placa pede.
    borda: float = 0.5

    def do_projeto(self, placa) -> "Regras":
        """Aplica o que o projeto do KiCad exige por cima do que a bancada exige.

        A folga entre redes fica a MAIOR das duas: 0,4 e o minimo que a fresa
        aguenta, mas se o projeto pede 0,5 (o blink blink pede, na classe
        Default) e a trilha sai a 0,45, o DRC do KiCad reprova a placa inteira -
        foram 26 violacoes na primeira vez que o botao aplicou um roteamento.
        """
        self.borda = placa.borda
        self.folga = max(self.folga, placa.folga)
        # Grade pelo tamanho da placa. 0,1 mm numa placa de 3 x 3 cm sao 90 mil
        # celulas por face; no interfone (103 x 82 mm) eram 860 mil, e um
        # roteamento levava 30 s - o sorteio nao andava. Fica a grade mais fina
        # que caiba em CELULAS_MAX; o nucleo poe meia celula de margem a mais nos
        # obstaculos quando a grade engrossa, entao a folga continua respeitada.
        area = max(placa.largura * placa.altura, 1.0)
        for passo in (0.1, 0.125, 0.15, 0.2, 0.25):
            self.grade = passo
            if area / (passo * passo) <= CELULAS_MAX:
                break
        return self

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
