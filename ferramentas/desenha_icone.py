# -*- coding: utf-8 -*-
"""Desenha o icone do plugin: placa fresada, trilha a 45 graus, via e a ponta V.

Desenha grande (512 px) e reduz com filtro bom, para o icone de 24 px da barra
do KiCad sair liso. Gera tambem a versao para tema escuro e a de 64 px que o
gerenciador de plugins (PCM) mostra.

    python ferramentas/desenha_icone.py
"""
import os

from PIL import Image, ImageDraw

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G = 512                       # tela de trabalho

PLACA = (22, 92, 64)          # FR-4 verde
PLACA_BORDA = (12, 60, 42)
COBRE = (232, 140, 52)
COBRE_ESCURO = (170, 92, 30)
FURO = (16, 40, 30)
FRESA = (214, 222, 230)
FRESA_SOMBRA = (86, 98, 112)


def s(v):
    return int(round(v * G / 100.0))


def trilha(d, pontos, largura, cor):
    pts = [(s(x), s(y)) for x, y in pontos]
    d.line(pts, fill=cor, width=s(largura), joint="curve")
    r = s(largura) / 2
    for x, y in pts:                       # pontas redondas
        d.ellipse((x - r, y - r, x + r, y + r), fill=cor)


def desenha(fundo_escuro=False):
    img = Image.new("RGBA", (G, G), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # placa: quadrado arredondado, deixando o canto de cima livre para a fresa
    d.rounded_rectangle((s(2), s(16), s(84), s(98)), radius=s(14),
                        fill=PLACA, outline=PLACA_BORDA, width=s(3))

    # trilha a 45 graus: entra pela esquerda, sobe na diagonal e chega na via
    caminho = [(10, 84), (28, 84), (48, 64), (48, 56)]
    trilha(d, caminho, 15, COBRE_ESCURO)          # sulco da fresa em volta
    trilha(d, caminho, 10, COBRE)
    trilha(d, [(10, 64), (20, 64), (30, 54)], 10, COBRE)

    # via: ilha grande com furo, como na bancada
    cx, cy, r = 48, 46, 14
    d.ellipse((s(cx - r), s(cy - r), s(cx + r), s(cy + r)), fill=COBRE,
              outline=COBRE_ESCURO, width=s(2.5))
    d.ellipse((s(cx - 5.5), s(cy - 5.5), s(cx + 5.5), s(cy + 5.5)), fill=FURO)

    # fresa de ponta V, inteira, no canto de cima a direita
    x0, x1, topo, base, ponta = 70, 96, 2, 36, 62
    meio = (x0 + x1) / 2.0
    d.rounded_rectangle((s(x0), s(topo), s(x1), s(base)), radius=s(3),
                        fill=FRESA, outline=FRESA_SOMBRA, width=s(2.5))
    d.polygon([(s(x0), s(base)), (s(x1), s(base)), (s(meio), s(ponta))],
              fill=FRESA, outline=FRESA_SOMBRA)
    d.line([(s(meio), s(base)), (s(meio), s(ponta - 2))], fill=FRESA_SOMBRA, width=s(3))
    for y in (12, 25):                            # helice
        d.line([(s(x0 + 1), s(y + 5)), (s(x1 - 1), s(y - 3))],
               fill=FRESA_SOMBRA, width=s(3.5))
    # cavaco de cobre saltando da ponta
    for dx, dy, rr in ((-9, 4, 2.4), (8, 6, 2.0), (-4, 10, 1.8)):
        x, y = meio + dx, ponta + dy
        d.ellipse((s(x - rr), s(y - rr), s(x + rr), s(y + rr)), fill=COBRE)

    if fundo_escuro:
        # no tema escuro a borda da placa some; um contorno claro devolve a forma
        d.rounded_rectangle((s(2), s(16), s(84), s(98)), radius=s(14),
                            outline=(90, 170, 130), width=s(2.5))
    return img


def salva(img, tam, nome):
    img.resize((tam, tam), Image.LANCZOS).save(os.path.join(RAIZ, nome))


if __name__ == "__main__":
    claro, escuro = desenha(False), desenha(True)
    salva(claro, 24, "icone.png")
    salva(escuro, 24, "icone_escuro.png")
    os.makedirs(os.path.join(RAIZ, "pcm"), exist_ok=True)
    salva(claro, 64, os.path.join("pcm", "icon.png"))
    salva(claro, 256, os.path.join("pcm", "icone_grande.png"))
    print("icones gerados")
