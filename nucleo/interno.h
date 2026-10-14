/* O que o nucleo compartilha entre os seus arquivos e nao exporta. */
#ifndef PERFCNC_INTERNO_H
#define PERFCNC_INTERNO_H

#include "perfcnc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PB_MAX_PLANOS 8

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
    /* Planos registrados (pb_define_planos): rede, face e, por celula, se a
     * celula fica dentro da zona do plano. */
    int n_planos;
    int plano_rede[PB_MAX_PLANOS];
    int plano_face[PB_MAX_PLANOS];
    unsigned char *plano_regiao[PB_MAX_PLANOS];
    double plano_raio[PB_MAX_PLANOS];    /* largura do raio termico, mm */
    /* Quanto a mascara do plano (celula livre no mapa inflado) ainda tem de
     * encolher, em celulas, para so sobrar o que o KiCad de fato preenche - ver
     * pb_define_planos. */
    int plano_erosao[PB_MAX_PLANOS];
    double plano_alivio[PB_MAX_PLANOS];  /* vao do alivio termico, mm */
    double plano_espessura[PB_MAX_PLANOS];
    double plano_folga[PB_MAX_PLANOS];   /* folga efetiva da zona, mm */

    /* Varredura de alcance (ver alcanca() no roteador): carimbo por estado
     * celula x face, como o do A*, e a fila. */
    unsigned *bfs_visto;
    unsigned bfs_marca;
    int *bfs_fila;

    /* Pedido de parar de quem chamou (o placar do posicionador); NULL = nunca.
     * A busca confere de tempos em tempos e desiste. */
    volatile int *parar;

    /* Alvos da busca atual marcados na grade (celula x face), com o numero da
     * busca: "cheguei?" vira uma consulta em vez de varrer centenas de celulas
     * de pad a cada passo. */
    unsigned *alvo_marca;

    /* Vias ja feitas neste mapa (coluna, linha, rede). Um caminho pode PASSAR
     * por via da propria rede, mas nao pode criar outra colada nela: duas
     * ligacoes de GND poem a via no mesmo ponto e o KiCad acusa furos
     * sobrepostos (holes_co_located). */
    int *via_lista;
    int n_via_lista, cap_via_lista;

    /* Rip-up dentro da busca (como o MazeRipupResolver do Freerouting): com
     * rasgo > 0, o A* pode atravessar cobre de OUTRA rede pagando isto por celula.
     * So cobre de trilha/via - a celula tem de estar livre em `rasgo_base`, o
     * mapa antes das trilhas - e nunca de rede com plano. */
    double rasgo;
    const int *rasgo_base;

    float *g;
    int *veio;
    unsigned *carimbo;   /* 2*busca: aberto nesta busca; 2*busca+1: fechado */
    unsigned busca;
};

int pb_livre_para(const PbMapa *m, int c, int l, int face, int rede);

/* Esvazia o mapa para reuso (mesma placa, mesmo tamanho): o posicionador roteia
 * centenas de posicoes, e alocar e preencher dezenas de MB por posicao, em
 * varios nucleos ao mesmo tempo, era o que travava o sorteio em placa grande. */
void pb_reinicia(PbMapa *m);

/* Pode haver via desta rede em (c, l)? Nao se ja houver outra da mesma rede a
 * menos de broca + folga - a nao ser exatamente no mesmo ponto (reuso). */
int pb_via_permitida(const PbMapa *m, int c, int l, int rede);

/* Registra uma via no mapa (ignora a repetida). */
void pb_anota_via(PbMapa *m, int c, int l, int rede);

/* Quanto cada obstaculo e inflado no mapa, mm: meia trilha + folga + a margem
 * da grade. Celula livre = centro a pelo menos isto de qualquer cobre alheio. */
double pb_inflacao(const PbMapa *m);

/* Celula (indice linha*colunas+coluna) e plano DE VERDADE: livre, dentro da
 * zona e longe o bastante de cobre alheio para sobreviver a espessura minima. */
int pb_e_plano(const PbMapa *m, int ip, int cel);

#endif
