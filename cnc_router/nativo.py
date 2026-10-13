# -*- coding: utf-8 -*-
"""Ponte com o nucleo em C. Traduz, nao decide.

Toda regra mora no C. Aqui so se empacota numero e se desempacota resposta - se
alguma decisao aparecer neste arquivo, ela vai divergir do C mais cedo ou mais
tarde, que foi o defeito mais caro do projeto anterior: o termo de espalhamento
viveu meses so no Python enquanto quem posicionava de verdade era o C.
"""
from __future__ import annotations

import ctypes
import os

ABI_ESPERADA = 1

CIRCULO, CAPSULA, RETANGULO, POLIGONO, FORA_DE, SEGMENTO = 0, 1, 2, 3, 4, 5
CIMA, BAIXO, AMBAS = 1, 2, 3
LIVRE, DE_NINGUEM = 0, -1


class PbRegras(ctypes.Structure):
    _fields_ = [
        ("trilha", ctypes.c_double), ("folga", ctypes.c_double),
        ("grade", ctypes.c_double), ("via_ilha", ctypes.c_double),
        ("broca", ctypes.c_double),
        ("custo_curva", ctypes.c_double), ("custo_via", ctypes.c_double),
        ("peso_cima", ctypes.c_double),
        ("salto_via", ctypes.c_int), ("orcamento", ctypes.c_long),
    ]


class PbForma(ctypes.Structure):
    _fields_ = [
        ("forma", ctypes.c_int), ("rede", ctypes.c_int), ("faces", ctypes.c_int),
        ("x", ctypes.c_double), ("y", ctypes.c_double),
        ("larg", ctypes.c_double), ("alt", ctypes.c_double),
        ("giro", ctypes.c_double), ("folga_extra", ctypes.c_double),
        ("n_pontos", ctypes.c_int), ("pontos", ctypes.POINTER(ctypes.c_double)),
    ]


class NucleoAusente(RuntimeError):
    pass


def _carrega():
    pasta = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "nucleo")
    for nome in ("perfcnc.dll", "perfcnc.so", "libperfcnc.so"):
        caminho = os.path.join(pasta, nome)
        if not os.path.exists(caminho):
            continue
        lib = ctypes.CDLL(caminho)
        lib.pb_abi.restype = ctypes.c_int
        achada = lib.pb_abi()
        if achada != ABI_ESPERADA:
            # Binario de outra versao LE OS CAMPOS TROCADOS e devolve numero
            # plausivel e errado. No projeto anterior isso custou horas: a resposta
            # parecia certa. Falhar aqui, na cara, e o unico jeito honesto.
            raise NucleoAusente(
                "nucleo em %s e da ABI %d, esperada %d - recompile com nucleo/build.sh"
                % (caminho, achada, ABI_ESPERADA))
        _assina(lib)
        return lib
    raise NucleoAusente("nucleo nao compilado: rode nucleo/build.sh")


def _assina(lib):
    P = ctypes.POINTER
    lib.pb_cria.restype = ctypes.c_void_p
    lib.pb_cria.argtypes = [P(PbRegras), ctypes.c_double, ctypes.c_double,
                            ctypes.c_double, ctypes.c_double]
    lib.pb_destroi.argtypes = [ctypes.c_void_p]
    lib.pb_pinta.argtypes = [ctypes.c_void_p, P(PbForma), ctypes.c_int]
    for nome in ("pb_colunas", "pb_linhas"):
        getattr(lib, nome).restype = ctypes.c_int
        getattr(lib, nome).argtypes = [ctypes.c_void_p]
    lib.pb_dono.restype = ctypes.c_int
    lib.pb_dono.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 3
    for nome in ("pb_coluna_de", "pb_linha_de"):
        getattr(lib, nome).restype = ctypes.c_int
        getattr(lib, nome).argtypes = [ctypes.c_void_p, ctypes.c_double]
    for nome in ("pb_x_de", "pb_y_de"):
        getattr(lib, nome).restype = ctypes.c_double
        getattr(lib, nome).argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.pb_roteia.restype = ctypes.c_int
    lib.pb_roteia.argtypes = [ctypes.c_void_p, ctypes.c_int,
                              P(ctypes.c_int), ctypes.c_int,
                              P(ctypes.c_int), ctypes.c_int,
                              P(ctypes.c_int), ctypes.c_int]


_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        _LIB = _carrega()
    return _LIB


def regras_para_c(regras, custos=None, salto_via=5, orcamento=3000000) -> PbRegras:
    c = custos
    return PbRegras(
        trilha=regras.trilha, folga=regras.folga, grade=regras.grade,
        via_ilha=regras.via_ilha, broca=regras.broca,
        custo_curva=getattr(c, "curva", 6.0),
        custo_via=getattr(c, "via", 40.0),
        peso_cima=getattr(c, "face_de_cima", 1.15),
        salto_via=salto_via, orcamento=orcamento,
    )


class Mapa:
    """Dono do mapa em C. Fecha sozinho."""

    def __init__(self, regras_c: PbRegras, x0, y0, largura, altura):
        self._lib = lib()
        self._p = self._lib.pb_cria(ctypes.byref(regras_c), x0, y0, largura, altura)
        if not self._p:
            raise NucleoAusente("o nucleo nao conseguiu criar o mapa")
        self._vivo = True
        self._guardados = []      # mantem os poligonos vivos enquanto o mapa viver

    def pinta(self, formas):
        vetor = (PbForma * len(formas))(*formas)
        self._lib.pb_pinta(self._p, vetor, len(formas))

    def guarda(self, buffer):
        self._guardados.append(buffer)

    @property
    def colunas(self):
        return self._lib.pb_colunas(self._p)

    @property
    def linhas(self):
        return self._lib.pb_linhas(self._p)

    def dono(self, c, l, face):
        return self._lib.pb_dono(self._p, c, l, face)

    def para_grade(self, x, y):
        return (self._lib.pb_coluna_de(self._p, x), self._lib.pb_linha_de(self._p, y))

    def para_mm(self, c, l):
        return (self._lib.pb_x_de(self._p, c), self._lib.pb_y_de(self._p, l))

    def roteia(self, rede, origens, alvos, max_passos=200000):
        o = (ctypes.c_int * (len(origens) * 3))()
        for i, (c, l, f) in enumerate(origens):
            o[i * 3], o[i * 3 + 1], o[i * 3 + 2] = c, l, f
        a = (ctypes.c_int * (len(alvos) * 3))()
        for i, (c, l, f) in enumerate(alvos):
            a[i * 3], a[i * 3 + 1], a[i * 3 + 2] = c, l, f
        saida = (ctypes.c_int * (max_passos * 4))()
        n = self._lib.pb_roteia(self._p, rede, o, len(origens), a, len(alvos),
                                saida, max_passos * 4)
        if n < 0:
            return None
        return [(saida[i * 4], saida[i * 4 + 1], saida[i * 4 + 2], saida[i * 4 + 3])
                for i in range(n)]

    def __del__(self):
        if getattr(self, "_vivo", False):
            self._lib.pb_destroi(self._p)
            self._vivo = False
