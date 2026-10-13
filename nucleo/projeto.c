/* Roteia a placa inteira: ordem das redes, arvore de ligacao e repintura.
 *
 * A ordem importa. Quem roteia primeiro escolhe o caminho bom e deixa o resto do
 * espaco para os outros, entao as redes dificeis vao primeiro - dificil aqui e
 * "tem muitos pads" e "os pads estao longe uns dos outros". Deixar uma rede de 6
 * pads por ultimo e pedir para ela nao fechar.
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
} Rede;

static int compara_redes(const void *a, const void *b)
{
    const Rede *ra = (const Rede *)a, *rb = (const Rede *)b;
    if (ra->n != rb->n) return rb->n - ra->n;    /* mais pads primeiro */
    return ra->rede - rb->rede;                   /* desempate estavel */
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

int pb_roteia_tudo(PbMapa *m, const PbAlvo *alvos, int n_alvos,
                   PbSeg *segs, int max_segs,
                   PbViaOut *vias, int max_vias,
                   PbAberta *abertas, int max_abertas,
                   int *n_segs_out, int *n_vias_out, int *n_abertas_out)
{
    Rede *redes = NULL;
    int n_redes = 0, i, j, ligadas = 0;
    int n_segs = 0, n_vias = 0, n_abertas = 0;
    int *caminho = NULL, max_caminho = 200000;
    int *origens = NULL, n_origens;
    char *ligado = NULL;

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

    qsort(redes, (size_t)n_redes, sizeof(Rede), compara_redes);

    caminho = (int *)malloc((size_t)max_caminho * 4 * sizeof(int));
    origens = (int *)malloc((size_t)max_caminho * 3 * sizeof(int));
    ligado = (char *)malloc((size_t)n_alvos);

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
                ligado[melhor] = 1;
                restantes--;
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

    for (j = 0; j < n_redes; j++) free(redes[j].indices);
    free(redes); free(caminho); free(origens); free(ligado);

    *n_segs_out = n_segs;
    *n_vias_out = n_vias;
    *n_abertas_out = n_abertas;
    return ligadas;
}
