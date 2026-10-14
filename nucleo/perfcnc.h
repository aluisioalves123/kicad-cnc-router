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
#define PB_ABI 9   /* 9: planos (PbPlano, pb_define_planos, pb_posiciona) */
/* (o corpo agora e o courtyard - mesma struct, mesma ABI) */

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
    double borda;        /* folga cobre-borda da placa, mm (Board Setup) */

    int salto_via;       /* a via so e tentada de N em N celulas */
    long orcamento;      /* teto de nos por busca; 0 = sem teto */
    int tentativas;      /* passadas de rip-up & reroute por roteamento; 0 = 1 */
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

/* Plano: zona de cobre com rede (o plano de GND). Na fresa e o cobre que sobra
 * depois da isolacao, ligado a uma rede. O roteador nao faz trilha onde o plano
 * ja liga: so costura os pedacos que as outras trilhas separaram.
 * `face` e 0 (cima) ou 1 (baixo); so o plano de BAIXO liga pad - em cima, ligar
 * no pad seria solda do lado dos componentes. */
typedef struct {
    int rede;
    int face;
    int n_pontos;
    const double *pontos;
} PbPlano;

/* Celula de chegada de um pad: sempre na face de baixo, que e a regra do projeto. */
typedef struct { int rede; int c, l; } PbAlvo;

typedef struct { int c1, l1, c2, l2, face, rede; } PbSeg;
typedef struct { int c, l, rede; } PbViaOut;
typedef struct { int rede, c, l; } PbAberta;

/* Uma peca para o posicionador. `movel` = 0 para travada (cadeado do KiCad) e
 * para mecanica (furo de fixacao: sem rede, nao entra em roteamento, e mudar o
 * lugar dele muda onde a placa parafusa). */
typedef struct {
    double x, y, giro;   /* origem do footprint no mundo */
    double larg, alt;    /* corpo no referencial DA PECA (giro zero) */
    double cx, cy;       /* centro do corpo em relacao a origem, ref. da peca */
    int movel;
    /* Pontos do courtyard no referencial da peca (pares x,y). O corpo e o casco
     * convexo deles; sem pontos, vale a caixa larg x alt em (cx, cy). */
    int n_corpo;
    const double *corpo;
    int papel;           /* PB_PAPEL_* */
} PbPeca;

/* Papel da peca para o posicionador. Conector vai para a PERIFERIA do circuito
 * (nenhuma peca entre ele e a borda - nao encostado nela): fio saindo do meio da
 * placa e feio e atrapalha a montagem. Mecanica (furo de fixacao) nao conta como
 * peca no caminho de ninguem. */
#define PB_PAPEL_COMUM     0
#define PB_PAPEL_CONECTOR  1
#define PB_PAPEL_MECANICA  2

/* Pad no referencial da PECA, antes do giro dela. */
typedef struct {
    int peca;
    int rede;
    int forma;           /* PB_CIRCULO / PB_CAPSULA / PB_RETANGULO */
    double dx, dy, giro_local;
    double larg, alt;
} PbPadC;

/* Area de regra do KiCad. As proibicoes sao independentes: ha area que barra so
 * via, ou so footprint. */
typedef struct {
    int faces_trilha;    /* PB_CIMA|PB_BAIXO onde barra trilha; 0 = nao barra */
    int sem_footprint;
    int n_pontos;
    const double *pontos;
} PbProibida;

typedef struct {
    int abertas_antes;   /* da posicao de entrada */
    int abertas_depois;
    int vias;
    double fio;          /* mm */
    int vencedor;        /* 0 = a posicao de entrada continuou sendo a melhor */
    int descartados;     /* candidatos ilegais: peca empilhada, fora da placa... */
    int julgados;        /* candidatos legais que foram roteados */
    int nucleos;
    double segundos;
    /* Quem chamou poe 1 aqui para parar: cada nucleo termina o sorteio em que
     * esta e o resultado sai com a melhor ate entao. Enquanto roda, o C mantem
     * julgados/descartados/abertas_depois/vias/vencedor atualizados - e o que a
     * janela do KiCad mostra. */
    volatile int parar;
    /* Quanto de peca ainda fica na frente dos conectores na vencedora, somado em
     * mm (multiplos de 0,5). Zero = todos na periferia. */
    double conectores_mm;
} PbPlacar;

#ifdef __cplusplus
extern "C" {
#endif

PB_EXPORT int pb_abi(void);

PB_EXPORT PbMapa *pb_cria(const PbRegras *regras,
                          double x0, double y0, double largura, double altura);
PB_EXPORT void pb_destroi(PbMapa *m);

PB_EXPORT void pb_pinta(PbMapa *m, const PbForma *formas, int quantas);

/* Diz ao mapa onde ha plano. Chamar depois de pintar os obstaculos e antes de
 * pb_roteia_tudo. Devolve quantos planos ficaram registrados. */
PB_EXPORT int pb_define_planos(PbMapa *m, const PbPlano *planos, int quantos);

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
/* Roteia a placa inteira. Devolve quantas ligacoes fechou; as que nao fecharam
 * saem em `abertas`. */
PB_EXPORT int pb_roteia_tudo(PbMapa *m, const PbAlvo *alvos, int n_alvos,
                             PbSeg *segs, int max_segs,
                             PbViaOut *vias, int max_vias,
                             PbAberta *abertas, int max_abertas,
                             int *n_segs, int *n_vias, int *n_abertas);

/* Procura posicao para as pecas moveis e deixa o ROTEADOR escolher.
 *
 * Sorteia posicoes por recozimento, em `nucleos` threads (0 = metade), e roteia
 * cada uma; fica a que fechar mais ligacoes (empate: menos via, depois menos
 * fio). Para no primeiro candidato que fechar TUDO, ou em `segundos`, ou em
 * `candidatos` (0 = sem teto). O candidato zero e a posicao de entrada intocada,
 * entao o resultado nunca sai pior que ela. A vencedora volta em `pecas`. */
PB_EXPORT int pb_posiciona(const PbRegras *regras,
                           double x0, double y0, double largura, double altura,
                           const double *contorno, int n_contorno,
                           const PbProibida *proibidas, int n_proibidas,
                           const PbPlano *planos, int n_planos,
                           PbPeca *pecas, int n_pecas,
                           const PbPadC *pads, int n_pads,
                           int candidatos, int passos, unsigned long long semente,
                           double segundos, int nucleos,
                           PbPlacar *placar);

PB_EXPORT int pb_roteia(PbMapa *m, int rede,
                        const int *origens, int n_origens,
                        const int *alvos, int n_alvos,
                        int *saida, int max_saida);

#ifdef __cplusplus
}
#endif

#endif
