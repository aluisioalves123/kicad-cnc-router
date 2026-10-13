/* Roteia a placa inteira: ordem das redes, arvore de ligacao e repintura.
 *
 * A ordem importa. Quem roteia primeiro escolhe o caminho bom e deixa o resto do
 * espaco para os outros, entao as redes com mais pads vao primeiro - deixar uma
 * rede de 6 pads por ultimo e pedir para ela nao fechar. E quando a ordem ainda
 * assim deixa ligacao aberta, rasga e roteia de novo (ver pb_roteia_tudo).
 *
 * Cada trecho, assim que fecha, e PINTADO no mapa: as redes seguintes desviam
 * dele, e a propria rede ganha todo o cobre como ponto de partida. E assim que se
 * liga uma rede de verdade, em arvore, e nao em corrente.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "perfcnc.h"
#include "interno.h"

#define PB_PARADO 8

typedef struct {
    int rede;
    int n;
    int *indices;        /* posicoes em `alvos` */
    int falhas;          /* ligacoes desta rede que nao fecharam, somadas nas passadas */
    unsigned long long chave;   /* ordem entre redes com a mesma contagem de falhas */
} Rede;

static int compara_redes(const void *a, const void *b)
{
    const Rede *ra = (const Rede *)a, *rb = (const Rede *)b;
    if (ra->falhas != rb->falhas) return rb->falhas - ra->falhas;  /* quem falhou, antes */
    if (ra->chave != rb->chave) return ra->chave < rb->chave ? -1 : 1;
    return ra->rede - rb->rede;
}

/* Converte o caminho celula a celula em trechos retos e vias.
 *
 * Juntar os passos colineares aqui e o que faz a trilha sair lisa em vez de uma
 * escada de mil pedacinhos de 0,1 mm. E o primeiro passo SEMPRE "muda de direcao",
 * porque o inicio nao tem direcao nenhuma - sem a checagem de comprimento isso
 * gerava segmento degenerado: eram 21 em 69 na primeira placa inteira, lixo que
 * iria para o .kicad_pcb.
 */
static void em_segmentos(const int *caminho, int n, int rede,
                         PbSeg *segs, int *n_segs, int max_segs,
                         PbViaOut *vias, int *n_vias, int max_vias)
{
    int i, ic = 0, ia = 0;      /* inicio e anterior, indices no caminho */
    if (n <= 0) return;

    for (i = 1; i < n; i++) {
        const int *at = &caminho[i * 4];
        const int *an = &caminho[ia * 4];
        const int *in = &caminho[ic * 4];
        int trocou_face = at[2] != an[2];
        int mudou_dir = (at[3] != an[3]) && (at[3] != PB_PARADO);

        if (trocou_face) {
            if ((in[0] != an[0] || in[1] != an[1]) && *n_segs < max_segs) {
                PbSeg *s = &segs[(*n_segs)++];
                s->c1 = in[0]; s->l1 = in[1];
                s->c2 = an[0]; s->l2 = an[1];
                s->face = in[2]; s->rede = rede;
            }
            if (*n_vias < max_vias) {
                PbViaOut *v = &vias[(*n_vias)++];
                v->c = an[0]; v->l = an[1]; v->rede = rede;
            }
            ic = i;
        } else if (mudou_dir) {
            if ((in[0] != an[0] || in[1] != an[1]) && *n_segs < max_segs) {
                PbSeg *s = &segs[(*n_segs)++];
                s->c1 = in[0]; s->l1 = in[1];
                s->c2 = an[0]; s->l2 = an[1];
                s->face = in[2]; s->rede = rede;
            }
            ic = ia;
        }
        ia = i;
    }
    {
        const int *an = &caminho[ia * 4];
        const int *in = &caminho[ic * 4];
        if ((in[0] != an[0] || in[1] != an[1]) && *n_segs < max_segs) {
            PbSeg *s = &segs[(*n_segs)++];
            s->c1 = in[0]; s->l1 = in[1];
            s->c2 = an[0]; s->l2 = an[1];
            s->face = in[2]; s->rede = rede;
        }
    }
}

/* O cobre novo entra no mapa. */
static void pinta_trecho(PbMapa *m, const PbSeg *segs, int de, int ate,
                         const PbViaOut *vias, int vde, int vate)
{
    double folga = m->regras.trilha / 2.0 + m->regras.folga;
    int i;
    for (i = de; i < ate; i++) {
        PbForma f;
        memset(&f, 0, sizeof(f));
        f.forma = PB_SEGMENTO;
        f.rede = segs[i].rede;
        f.faces = (segs[i].face == 0) ? PB_CIMA : PB_BAIXO;
        f.x = pb_x_de(m, segs[i].c1); f.y = pb_y_de(m, segs[i].l1);
        f.larg = pb_x_de(m, segs[i].c2); f.alt = pb_y_de(m, segs[i].l2);
        f.giro = m->regras.trilha;          /* PB_SEGMENTO usa giro como largura */
        pb_pinta(m, &f, 1);
    }
    for (i = vde; i < vate; i++) {
        PbForma f;
        memset(&f, 0, sizeof(f));
        f.forma = PB_CIRCULO;
        f.rede = vias[i].rede;
        f.faces = PB_AMBAS;
        f.x = pb_x_de(m, vias[i].c); f.y = pb_y_de(m, vias[i].l);
        f.larg = m->regras.via_ilha;
        pb_pinta(m, &f, 1);
    }
    (void)folga;
}

/* Uma passada: roteia as redes na ordem em que estao em `redes`. Conta em cada
 * rede quantas ligacoes dela nao fecharam - e isso que a proxima passada usa
 * para decidir quem vai primeiro. */
static int uma_passada(PbMapa *m, const PbAlvo *alvos, int n_alvos,
                       Rede *redes, int n_redes,
                       PbSeg *segs, int max_segs,
                       PbViaOut *vias, int max_vias,
                       PbAberta *abertas, int max_abertas,
                       int *n_segs_out, int *n_vias_out, int *n_abertas_out,
                       int *caminho, int max_caminho, int *origens, char *ligado,
                       int teto)
{
    int i, j, ligadas = 0, n_origens;
    int n_segs = 0, n_vias = 0, n_abertas = 0;

    for (j = 0; j < n_redes; j++) {
        Rede *r = &redes[j];
        int restantes;
        if (r->n < 2) continue;

        memset(ligado, 0, (size_t)n_alvos);
        ligado[r->indices[0]] = 1;
        n_origens = 1;
        origens[0] = alvos[r->indices[0]].c;
        origens[1] = alvos[r->indices[0]].l;
        origens[2] = 1;                     /* chegada sempre pela face de baixo */
        restantes = r->n - 1;

        while (restantes > 0) {
            int melhor = -1, n;
            double melhor_d = 1e30;
            int destino[3];

            /* proximo alvo: o mais perto do que ja esta ligado */
            for (i = 0; i < r->n; i++) {
                int idx = r->indices[i];
                int k;
                if (ligado[idx]) continue;
                for (k = 0; k < n_origens; k++) {
                    double dc = alvos[idx].c - origens[k * 3];
                    double dl = alvos[idx].l - origens[k * 3 + 1];
                    double d = dc * dc + dl * dl;
                    if (d < melhor_d) { melhor_d = d; melhor = idx; }
                }
            }
            if (melhor < 0) break;

            destino[0] = alvos[melhor].c;
            destino[1] = alvos[melhor].l;
            destino[2] = 1;

            n = pb_roteia(m, r->rede, origens, n_origens, destino, 1,
                          caminho, max_caminho * 4);
            if (n < 0) {
                if (n_abertas < max_abertas) {
                    abertas[n_abertas].rede = r->rede;
                    abertas[n_abertas].c = alvos[melhor].c;
                    abertas[n_abertas].l = alvos[melhor].l;
                    n_abertas++;
                }
                r->falhas++;
                ligado[melhor] = 1;
                restantes--;
                /* ja perdeu para uma passada anterior: nao adianta terminar */
                if (teto >= 0 && n_abertas > teto) {
                    *n_segs_out = n_segs;
                    *n_vias_out = n_vias;
                    *n_abertas_out = n_abertas;
                    return ligadas;
                }
                continue;
            }

            {
                int s0 = n_segs, v0 = n_vias;
                em_segmentos(caminho, n, r->rede, segs, &n_segs, max_segs,
                             vias, &n_vias, max_vias);
                pinta_trecho(m, segs, s0, n_segs, vias, v0, n_vias);
            }

            /* todo o caminho vira ponto de partida do proximo trecho */
            for (i = 0; i < n && n_origens * 3 + 3 < max_caminho * 3; i++) {
                origens[n_origens * 3 + 0] = caminho[i * 4 + 0];
                origens[n_origens * 3 + 1] = caminho[i * 4 + 1];
                origens[n_origens * 3 + 2] = caminho[i * 4 + 2];
                n_origens++;
            }
            ligado[melhor] = 1;
            restantes--;
            ligadas++;
        }
    }

    *n_segs_out = n_segs;
    *n_vias_out = n_vias;
    *n_abertas_out = n_abertas;
    return ligadas;
}

static double fio_de(const PbMapa *m, const PbSeg *segs, int n)
{
    double total = 0.0;
    int i;
    for (i = 0; i < n; i++)
        total += hypot(pb_x_de(m, segs[i].c2) - pb_x_de(m, segs[i].c1),
                       pb_y_de(m, segs[i].l2) - pb_y_de(m, segs[i].l1));
    return total;
}

static unsigned long long embaralha(unsigned long long *s)
{
    unsigned long long x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

/* Rip-up & reroute, como na perfboard.
 *
 * Uma passada so e refem da ordem: a rede que roteia primeiro pega o caminho bom
 * e pode fechar a porta de outra que so tinha aquele. Entao, se sobrou ligacao
 * aberta, o mapa volta ao que era antes do cobre novo e roteia de novo, com quem
 * falhou NA FRENTE - e o resto embaralhado a partir da terceira passada, para a
 * busca nao andar em circulo. Fica a melhor passada: menos abertas, depois menos
 * via, depois menos fio.
 *
 * Deterministico: a mesma placa da sempre o mesmo resultado. Isso importa porque
 * o posicionador julga com esta funcao e o Python roteia a vencedora com ela de
 * novo - se as duas dessem resultados diferentes, o placar do juiz mentiria. */
int pb_roteia_tudo(PbMapa *m, const PbAlvo *alvos, int n_alvos,
                   PbSeg *segs, int max_segs,
                   PbViaOut *vias, int max_vias,
                   PbAberta *abertas, int max_abertas,
                   int *n_segs_out, int *n_vias_out, int *n_abertas_out)
{
    Rede *redes = NULL;
    int n_redes = 0, i, j, passada, passadas, melhor_passada = -1, ultima = -1;
    int *caminho = NULL, max_caminho = 200000;
    int *origens = NULL;
    char *ligado = NULL;
    int *base = NULL;
    size_t n_celulas = (size_t)m->colunas * m->linhas * 2;
    PbSeg *t_segs = NULL;
    PbViaOut *t_vias = NULL;
    PbAberta *t_abertas = NULL;
    int melhor_lig = 0, melhor_ns = 0, melhor_nv = 0, melhor_na = 0;
    double melhor_fio = 0.0;
    unsigned long long s = 0x2545F4914F6CDD1DULL;

    *n_segs_out = *n_vias_out = *n_abertas_out = 0;
    if (n_alvos <= 0) return 0;

    /* agrupa por rede */
    redes = (Rede *)calloc((size_t)n_alvos, sizeof(Rede));
    for (i = 0; i < n_alvos; i++) {
        int achou = -1;
        for (j = 0; j < n_redes; j++)
            if (redes[j].rede == alvos[i].rede) { achou = j; break; }
        if (achou < 0) { achou = n_redes++; redes[achou].rede = alvos[i].rede; }
        redes[achou].n++;
    }
    for (j = 0; j < n_redes; j++) {
        redes[j].indices = (int *)malloc((size_t)redes[j].n * sizeof(int));
        redes[j].n = 0;
    }
    for (i = 0; i < n_alvos; i++)
        for (j = 0; j < n_redes; j++)
            if (redes[j].rede == alvos[i].rede) {
                redes[j].indices[redes[j].n++] = i;
                break;
            }

    caminho = (int *)malloc((size_t)max_caminho * 4 * sizeof(int));
    origens = (int *)malloc((size_t)max_caminho * 3 * sizeof(int));
    ligado = (char *)malloc((size_t)n_alvos);
    base = (int *)malloc(n_celulas * sizeof(int));
    t_segs = (PbSeg *)malloc((size_t)(max_segs > 0 ? max_segs : 1) * sizeof(PbSeg));
    t_vias = (PbViaOut *)malloc((size_t)(max_vias > 0 ? max_vias : 1) * sizeof(PbViaOut));
    t_abertas = (PbAberta *)malloc((size_t)(max_abertas > 0 ? max_abertas : 1)
                                   * sizeof(PbAberta));
    memcpy(base, m->celulas, n_celulas * sizeof(int));

    passadas = m->regras.tentativas > 0 ? m->regras.tentativas : 1;
    for (passada = 0; passada < passadas; passada++) {
        int ns, nv, na, lig;
        double fio;

        /* ordem: quem falhou vai na frente; o resto, nas duas primeiras passadas,
         * por numero de pads (mais pads primeiro); da terceira em diante, sorteado */
        for (j = 0; j < n_redes; j++)
            redes[j].chave = passada < 2
                ? ((unsigned long long)(1000000 - redes[j].n) << 32)
                  | (unsigned)redes[j].rede
                : embaralha(&s);
        qsort(redes, (size_t)n_redes, sizeof(Rede), compara_redes);

        if (passada > 0) {
            memcpy(m->celulas, base, n_celulas * sizeof(int));
            m->soma_da_rede = -999999;
        }
        lig = uma_passada(m, alvos, n_alvos, redes, n_redes,
                          t_segs, max_segs, t_vias, max_vias, t_abertas, max_abertas,
                          &ns, &nv, &na, caminho, max_caminho, origens, ligado,
                          melhor_passada < 0 ? -1 : melhor_na);
        ultima = passada;
        fio = fio_de(m, t_segs, ns);
        if (melhor_passada < 0 || na < melhor_na
                || (na == melhor_na && nv < melhor_nv)
                || (na == melhor_na && nv == melhor_nv && fio < melhor_fio - 1e-6)) {
            melhor_passada = passada;
            melhor_na = na; melhor_nv = nv; melhor_ns = ns;
            melhor_fio = fio; melhor_lig = lig;
            memcpy(segs, t_segs, (size_t)ns * sizeof(PbSeg));
            memcpy(vias, t_vias, (size_t)nv * sizeof(PbViaOut));
            memcpy(abertas, t_abertas, (size_t)na * sizeof(PbAberta));
        }
        if (na == 0) break;
    }

    /* o mapa termina com o cobre da passada que ganhou, nao com o da ultima */
    if (ultima != melhor_passada) {
        memcpy(m->celulas, base, n_celulas * sizeof(int));
        m->soma_da_rede = -999999;
        pinta_trecho(m, segs, 0, melhor_ns, vias, 0, melhor_nv);
    }

    for (j = 0; j < n_redes; j++) free(redes[j].indices);
    free(redes); free(caminho); free(origens); free(ligado); free(base);
    free(t_segs); free(t_vias); free(t_abertas);

    *n_segs_out = melhor_ns;
    *n_vias_out = melhor_nv;
    *n_abertas_out = melhor_na;
    return melhor_lig;
}
