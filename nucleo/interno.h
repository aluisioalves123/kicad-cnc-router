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

    /* Rascunho do A*, um valor por estado (celula x face x direcao), alocado na
     * primeira busca e reaproveitado pelas seguintes.
     *
     * Antes cada busca alocava e preenchia ~2 milhoes de estados (27 MB) para
     * achar um caminho que visita uma fracao disso - umas 80 buscas por sorteio,
     * gigabytes de escrita, e 12 threads disputando a memoria rendiam o mesmo
     * que 4. O carimbo diz se o valor e DESTA busca: valor de busca velha conta
     * como nunca visto, entao nada precisa ser zerado entre uma e outra. */
    float *g;
    int *veio;
    unsigned *carimbo;   /* 2*busca: aberto nesta busca; 2*busca+1: fechado */
    unsigned busca;
};

int pb_livre_para(const PbMapa *m, int c, int l, int face, int rede);

#endif
