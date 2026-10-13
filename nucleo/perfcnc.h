/* Nucleo do roteador para fresagem caseira.
 *
 * Tudo o que decide cobre mora aqui: rasterizacao, A* e posicionamento. Do lado
 * do Python fica so a ponte com o pcbnew - ler o board e escrever trilha - porque
 * a API de plugin do KiCad e Python e nao ha como fugir disso.
 *
 * A CONFERENCIA fica de fora de proposito, em Python, medindo em milimetro. Ela ja
 * pegou quatro defeitos reais, dois deles no proprio conferidor. O valor dela vem
 * de ser uma segunda cabeca: se ela usar as funcoes de geometria daqui, deixa de
 * conferir e vira eco.
 */
#ifndef PERFCNC_H
#define PERFCNC_H

#ifdef _WIN32
#define PB_EXPORT __declspec(dllexport)
#else
#define PB_EXPORT __attribute__((visibility("default")))
#endif

/* Versao da interface entre o C e o Python.
 *
 * Sobe SEMPRE que qualquer struct ou assinatura daqui mudar. O Python confere
 * antes de chamar qualquer coisa e se recusa a usar binario de outra versao.
 *
 * Isto existe por cicatriz: no projeto anterior eu mudei a assinatura em C, o
 * Python chamou o binario antigo, e a resposta foi um numero plausivel e errado -
 * "15 pinos soltos" em toda configuracao. Demorei a desconfiar justamente porque
 * parecia certo. Binario incompativel tem de falhar na cara, nao mentir baixinho.
 */
#define PB_ABI 1

/* ------------------------------------------------------------------ formas */
#define PB_CIRCULO    0
#define PB_CAPSULA    1   /* oval do KiCad: retangulo com pontas redondas */
#define PB_RETANGULO  2
#define PB_POLIGONO   3
#define PB_FORA_DE    4   /* pinta tudo FORA do poligono: o contorno da placa */
#define PB_SEGMENTO   5   /* trilha ja existente */

/* Em que faces a forma vale. */
#define PB_CIMA   1
#define PB_BAIXO  2
#define PB_AMBAS  3

/* Dono de uma celula. */
#define PB_LIVRE        0
#define PB_DE_NINGUEM  -1

typedef struct {
    double trilha;       /* largura da trilha, mm */
    double folga;        /* afastamento entre cobres de redes diferentes, mm */
    double grade;        /* passo da rasterizacao, mm */
    double via_ilha;     /* diametro do cobre da via, mm */
    double broca;        /* furo, mm */

    double custo_curva;  /* por mudanca de direcao, em unidades de celula */
    double custo_via;
    double peso_cima;    /* andar pela face de cima custa isto vezes mais */

    int salto_via;       /* a via so e tentada de N em N celulas */
    long orcamento;      /* teto de nos por busca; 0 = sem teto */
} PbRegras;

typedef struct {
    int forma;
    int rede;            /* PB_DE_NINGUEM para obstaculo de todos */
    int faces;           /* PB_CIMA | PB_BAIXO */
    double x, y;         /* centro, mm (ou primeiro ponto, em poligono) */
    double larg, alt;    /* mm; em PB_SEGMENTO: ponta final em (larg, alt) */
    double giro;         /* graus */
    double folga_extra;  /* inflar alem da folga padrao, mm */
    int n_pontos;        /* PB_POLIGONO / PB_FORA_DE: quantos pares em `pontos` */
    const double *pontos;
} PbForma;

typedef struct PbMapa PbMapa;

#ifdef __cplusplus
extern "C" {
#endif

PB_EXPORT int pb_abi(void);

PB_EXPORT PbMapa *pb_cria(const PbRegras *regras,
                          double x0, double y0, double largura, double altura);
PB_EXPORT void pb_destroi(PbMapa *m);

PB_EXPORT void pb_pinta(PbMapa *m, const PbForma *formas, int quantas);

PB_EXPORT int pb_colunas(const PbMapa *m);
PB_EXPORT int pb_linhas(const PbMapa *m);
PB_EXPORT int pb_dono(const PbMapa *m, int coluna, int linha, int face);

/* Converte mm <-> celula, para o Python nao precisar repetir a conta. */
PB_EXPORT int pb_coluna_de(const PbMapa *m, double x);
PB_EXPORT int pb_linha_de(const PbMapa *m, double y);
PB_EXPORT double pb_x_de(const PbMapa *m, int coluna);
PB_EXPORT double pb_y_de(const PbMapa *m, int linha);

/* Caminho mais barato de qualquer origem ate qualquer destino.
 *
 * `origens` e `alvos` sao triplas (coluna, linha, face). A saida recebe quadras
 * (coluna, linha, face, direcao_de_chegada). Devolve quantos passos escreveu, ou
 * -1 se nao achou caminho, ou -2 se a saida era pequena demais.
 */
PB_EXPORT int pb_roteia(PbMapa *m, int rede,
                        const int *origens, int n_origens,
                        const int *alvos, int n_alvos,
                        int *saida, int max_saida);

#ifdef __cplusplus
}
#endif

#endif
