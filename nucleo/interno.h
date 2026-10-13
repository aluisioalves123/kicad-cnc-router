/* O que o nucleo compartilha entre os seus arquivos e nao exporta. */
#ifndef PERFCNC_INTERNO_H
#define PERFCNC_INTERNO_H

#include "perfcnc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct PbMapa {
    PbRegras regras;
    double x0, y0;
    int colunas, linhas;
    int *celulas;        /* (linha * colunas + coluna) * 2 + face */

    /* Imagem integral do que esta bloqueado para UMA rede, uma por face.
     *
     * Serve para responder "a ilha da via cabe aqui" em quatro somas em vez de
     * varrer ~800 celulas. Vale so para `soma_da_rede`; mudou a rede ou mudou o
     * mapa, tem de refazer. */
    int *soma[2];
    int soma_da_rede;
};

int pb_livre_para(const PbMapa *m, int c, int l, int face, int rede);

#endif
