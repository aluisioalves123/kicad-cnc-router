/* A* sobre a grade, nas 8 direcoes, com via e chegada obrigatoria por baixo.
 *
 * Numa grade fina a diagonal E o segmento a 45 graus - nao precisa de geometria
 * continua nem de pos-processamento para "arredondar" o caminho. Foi o que fez
 * descartar o A* continuo, que seria mais pesado e sem ganho.
 *
 * O estado carrega a DIRECAO DE CHEGADA, nao so a celula. Sem isso nao da para
 * cobrar por curva, e trilha fresada cheia de degrau fica feia e fragil: canto
 * vivo e onde a fresa deixa rebarba e onde o cobre descola.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "perfcnc.h"
#include "interno.h"

#define PB_PARADO 8          /* "cheguei sem direcao": inicio e saida de via */
#define PB_ESTADOS 9         /* 8 direcoes + parado */
#define RAIZ2 1.41421356237309504880

static const int DC[8] = { 1, 1, 0, -1, -1, -1,  0,  1 };
static const int DL[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };

/* ------------------------------------------------------------------ heap */

typedef struct {
    double f, g;
    int estado;
} No;

typedef struct {
    No *v;
    int n, cap;
} Heap;

static void heap_push(Heap *h, double f, double g, int estado)
{
    int i;
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 1024;
        h->v = (No *)realloc(h->v, (size_t)h->cap * sizeof(No));
    }
    i = h->n++;
    h->v[i].f = f; h->v[i].g = g; h->v[i].estado = estado;
    while (i > 0) {
        int pai = (i - 1) / 2;
        if (h->v[pai].f <= h->v[i].f) break;
        { No t = h->v[pai]; h->v[pai] = h->v[i]; h->v[i] = t; }
        i = pai;
    }
}

static int heap_pop(Heap *h, No *saida)
{
    int i = 0;
    if (h->n == 0) return 0;
    *saida = h->v[0];
    h->v[0] = h->v[--h->n];
    for (;;) {
        int e = i * 2 + 1, d = e + 1, menor = i;
        if (e < h->n && h->v[e].f < h->v[menor].f) menor = e;
        if (d < h->n && h->v[d].f < h->v[menor].f) menor = d;
        if (menor == i) break;
        { No t = h->v[menor]; h->v[menor] = h->v[i]; h->v[i] = t; }
        i = menor;
    }
    return 1;
}

/* ------------------------------------------------------------------ via */

/* Monta a imagem integral do que esta bloqueado para esta rede, uma vez por
 * busca. A ilha da via e um disco de 2,5 mm: conferir celula por celula custa
 * ~800 consultas, e o A* perguntava isso em CADA celula que expandia. */
static void prepara_via(PbMapa *m, int rede)
{
    int face, c, l, cols = m->colunas, lins = m->linhas;
    if (m->soma_da_rede == rede) return;
    for (face = 0; face < 2; face++) {
        size_t tam = (size_t)(cols + 1) * (lins + 1);
        if (!m->soma[face])
            m->soma[face] = (int *)malloc(tam * sizeof(int));
        memset(m->soma[face], 0, tam * sizeof(int));
        for (l = 0; l < lins; l++) {
            int corrido = 0;
            for (c = 0; c < cols; c++) {
                int v = m->celulas[(l * cols + c) * 2 + face];
                corrido += (v == PB_LIVRE || v == rede) ? 0 : 1;
                m->soma[face][(l + 1) * (cols + 1) + c + 1] =
                    m->soma[face][l * (cols + 1) + c + 1] + corrido;
            }
        }
    }
    m->soma_da_rede = rede;
}

static int bloqueados(const int *soma, int cols, int c0, int l0, int c1, int l1)
{
    return soma[(l1 + 1) * (cols + 1) + c1 + 1]
         - soma[l0 * (cols + 1) + c1 + 1]
         - soma[(l1 + 1) * (cols + 1) + c0]
         + soma[l0 * (cols + 1) + c0];
}

/* A resposta tem de ser EXATA, e a ilha e um disco. O quadrado que envolve o
 * disco e 35% mais exigente: trocar isso por velocidade zerou todas as vias da
 * placa de teste. Entao o quadrado de dentro resolve o nao, o de fora resolve o
 * sim, e so o que sobra - pouco - paga o preco do disco celula a celula. */
static int cabe_via(PbMapa *m, int c, int l, int rede)
{
    double raio_mm = m->regras.via_ilha / 2.0 + m->regras.folga;
    int fora = (int)ceil(raio_mm / m->regras.grade);
    int dentro = (int)(fora / RAIZ2);
    int face, dc, dl;

    if (c - fora < 0 || l - fora < 0 ||
        c + fora >= m->colunas || l + fora >= m->linhas)
        return 0;

    for (face = 0; face < 2; face++) {
        const int *soma = m->soma[face];
        if (bloqueados(soma, m->colunas, c - dentro, l - dentro,
                       c + dentro, l + dentro))
            return 0;
        if (!bloqueados(soma, m->colunas, c - fora, l - fora, c + fora, l + fora))
            continue;                      /* quadrado de fora limpo: passa */
        for (dl = -fora; dl <= fora; dl++)
            for (dc = -fora; dc <= fora; dc++) {
                if (dc * dc + dl * dl > fora * fora) continue;
                if (!pb_livre_para(m, c + dc, l + dl, face, rede)) return 0;
            }
    }
    return 1;
}

/* ------------------------------------------------------------------ busca */

static double estimativa(int c, int l, const int *alvos, int n)
{
    double melhor = 1e30;
    int i;
    for (i = 0; i < n; i++) {
        int dc = abs(alvos[i * 3] - c), dl = abs(alvos[i * 3 + 1] - l);
        int menor = dc < dl ? dc : dl;
        double d = RAIZ2 * menor + abs(dc - dl);
        if (d < melhor) melhor = d;
    }
    return melhor;
}

int pb_roteia(PbMapa *m, int rede,
              const int *origens, int n_origens,
              const int *alvos, int n_alvos,
              int *saida, int max_saida)
{
    int cols = m->colunas, lins = m->linhas;
    size_t total = (size_t)cols * lins * 2 * PB_ESTADOS;
    float *g;
    int *veio;
    unsigned *carimbo, aberto, fechado;
    Heap h = { NULL, 0, 0 };
    long gastos = 0;
    int achou = -1, i, n = 0;

    if (n_origens <= 0 || n_alvos <= 0) return -1;
    prepara_via(m, rede);

    if (!m->carimbo) {
        m->g = (float *)malloc(total * sizeof(float));
        m->veio = (int *)malloc(total * sizeof(int));
        m->carimbo = (unsigned *)calloc(total, sizeof(unsigned));
        m->busca = 0;
        if (!m->g || !m->veio || !m->carimbo) {
            free(m->g); free(m->veio); free(m->carimbo);
            m->g = NULL; m->veio = NULL; m->carimbo = NULL;
            return -1;
        }
    }
    if (m->busca >= 0x7FFFFFF0u) {          /* o carimbo deu a volta: zera uma vez */
        memset(m->carimbo, 0, total * sizeof(unsigned));
        m->busca = 0;
    }
    m->busca++;
    g = m->g; veio = m->veio; carimbo = m->carimbo;
    aberto = m->busca * 2;
    fechado = aberto + 1;
/* g do estado nesta busca; estado que esta busca nao tocou vale infinito */
#define G_DE(e) (carimbo[e] >= aberto ? g[e] : 1e30f)

    for (i = 0; i < n_origens; i++) {
        int c = origens[i * 3], l = origens[i * 3 + 1], f = origens[i * 3 + 2];
        int est = ((l * cols + c) * 2 + f) * PB_ESTADOS + PB_PARADO;
        if (c < 0 || l < 0 || c >= cols || l >= lins) continue;
        g[est] = 0.0f;
        veio[est] = -1;
        carimbo[est] = aberto;
        heap_push(&h, estimativa(c, l, alvos, n_alvos), 0.0, est);
    }

    for (;;) {
        No no;
        int est, cel, face, dir, c, l, d;
        double gcur, peso;

        if (!heap_pop(&h, &no)) break;
        if (m->regras.orcamento > 0 && ++gastos > m->regras.orcamento) break;

        est = no.estado;
        if (carimbo[est] == fechado) continue;
        carimbo[est] = fechado;
        gcur = no.g;

        dir = est % PB_ESTADOS;
        cel = est / PB_ESTADOS;
        face = cel % 2;
        cel /= 2;
        c = cel % cols;
        l = cel / cols;

        for (i = 0; i < n_alvos; i++) {
            if (alvos[i * 3] == c && alvos[i * 3 + 1] == l
                    && alvos[i * 3 + 2] == face) {
                achou = est;
                break;
            }
        }
        if (achou >= 0) break;

        peso = (face == 0) ? m->regras.peso_cima : 1.0;

        for (d = 0; d < 8; d++) {
            int nc = c + DC[d], nl = l + DL[d], prox;
            double custo, ng;
            if (!pb_livre_para(m, nc, nl, face, rede)) continue;
            custo = ((DC[d] && DL[d]) ? RAIZ2 : 1.0) * peso;
            if (dir != PB_PARADO && dir != d) {
                int giro = abs(d - dir);
                if (giro > 4) giro = 8 - giro;
                /* 45 graus custa menos que 90: a trilha sai mais lisa */
                custo += m->regras.custo_curva * (giro == 1 ? 1.0 : 2.0);
            }
            prox = ((nl * cols + nc) * 2 + face) * PB_ESTADOS + d;
            ng = gcur + custo;
            if (ng < G_DE(prox) - 1e-4) {
                g[prox] = (float)ng;
                veio[prox] = est;
                if (carimbo[prox] != fechado) carimbo[prox] = aberto;
                heap_push(&h, ng + estimativa(nc, nl, alvos, n_alvos), ng, prox);
            }
        }

        /* via: troca de face no mesmo lugar. Nao basta a celula estar livre - a
           ilha inteira tem de caber, nas duas faces. Tratar a via como um PONTO
           poe duas vias de redes diferentes a 2,1 mm uma da outra, com ilha de
           2,5: curto. */
        if (m->regras.salto_via <= 1
                || (c % m->regras.salto_via == 0 && l % m->regras.salto_via == 0)) {
            int outra = 1 - face;
            if (cabe_via(m, c, l, rede)) {
                int prox = ((l * cols + c) * 2 + outra) * PB_ESTADOS + PB_PARADO;
                double ng = gcur + m->regras.custo_via;
                if (ng < G_DE(prox) - 1e-4) {
                    g[prox] = (float)ng;
                    veio[prox] = est;
                    if (carimbo[prox] != fechado) carimbo[prox] = aberto;
                    heap_push(&h, ng + estimativa(c, l, alvos, n_alvos), ng, prox);
                }
            }
        }
    }

    if (achou >= 0) {
        int est = achou;
        /* conta primeiro, para saber se cabe antes de escrever */
        for (n = 0; est >= 0; est = veio[est]) n++;
        if (n * 4 > max_saida) {
            free(h.v);
            return -2;
        }
        est = achou;
        for (i = n - 1; i >= 0; i--) {
            int cel = est / PB_ESTADOS;
            saida[i * 4 + 3] = est % PB_ESTADOS;
            saida[i * 4 + 2] = cel % 2;
            cel /= 2;
            saida[i * 4 + 0] = cel % cols;
            saida[i * 4 + 1] = cel / cols;
            est = veio[est];
        }
    }

#undef G_DE
    free(h.v);
    return (achou >= 0) ? n : -1;
}
