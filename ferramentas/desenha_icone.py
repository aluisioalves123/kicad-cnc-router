# -*- coding: utf-8 -*-
"""Desenha o icone do plugin: o que um ROTEADOR faz, nas cores do KiCad.

Um barramento de tres trilhas azuis (face de baixo) dobrando a 45 graus, e uma
trilha vermelha (face de cima) que atravessa o barramento descendo e subindo por
duas vias - o pulo de face que o roteador faz para cruzar. Fundo escuro de
canvas do KiCad. A primeira versao tinha uma fresa e parecia gerador de G-code.

Desenha grande (512 px) e reduz com filtro bom, para o icone de 24 px da barra
do KiCad sair liso. Gera tambem a versao para tema escuro e a de 64 px que o
gerenciador de plugins (PCM) mostra.

    python ferramentas/desenha_icone.py
"""
import os

from PIL import Image, ImageDraw

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G = 512                       # tela de trabalho

FUNDO = (14, 24, 40)          # canvas escuro do KiCad
FUNDO_BORDA = (40, 58, 86)
BAIXO = (66, 132, 230)        # B.Cu
CIMA = (226, 62, 62)          # F.Cu
ILHA = (214, 170, 72)         # pad/via
FURO = (14, 24, 40)


def s(v):
    return int(round(v * G / 100.0))


def trilha(d, pontos, largura, cor):
    pts = [(s(x), s(y)) for x, y in pontos]
    d.line(pts, fill=cor, width=s(largura), joint="curve")
    r = s(largura) / 2
    for x, y in pts:                       # pontas redondas
        d.ellipse((x - r, y - r, x + r, y + r), fill=cor)


def ilha(d, x, y, r, furo):
    d.ellipse((s(x - r), s(y - r), s(x + r), s(y + r)), fill=ILHA)
    d.ellipse((s(x - furo), s(y - furo), s(x + furo), s(y + furo)), fill=FURO)


def desenha(fundo_escuro=False):
    img = Image.new("RGBA", (G, G), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    d.rounded_rectangle((s(1), s(1), s(99), s(99)), radius=s(18), fill=FUNDO,
                        outline=(120, 150, 196) if fundo_escuro else FUNDO_BORDA,
                        width=s(2.5))

    # barramento na face de baixo: tres trilhas paralelas, dobra a 45 graus
    passo = 15
    for k in range(3):
        y0 = 22 + passo * k
        y1 = y0 + 26
        trilha(d, [(14, y0), (36, y0), (62, y1 - 0), (86, y1)], 8.5, BAIXO)
    for k in range(3):                     # pads nas pontas
        y0 = 22 + passo * k
        ilha(d, 14, y0, 6.5, 2.6)
        ilha(d, 86, y0 + 26, 6.5, 2.6)

    # face de cima: cruza o barramento entre duas vias
    vx, va, vb = 48, 14, 86
    trilha(d, [(vx, va), (vx, vb)], 8.5, CIMA)
    ilha(d, vx, va, 7.5, 3.0)
    ilha(d, vx, vb, 7.5, 3.0)
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
