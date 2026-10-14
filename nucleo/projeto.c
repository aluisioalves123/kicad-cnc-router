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
    int plano;           /* 1 = tem plano: vai por ultimo, quando o plano ja se sabe */
} Rede;

static int compara_redes(const void *a, const void *b)
{
    const Rede *ra = (const Rede *)a, *rb = (const Rede *)b;
    /* rede de plano por ultimo: o plano e o cobre que SOBRA, e so da para saber
     * o que sobra depois que as outras redes passaram */
    if (ra->plano != rb->plano) return ra->plano - rb->plano;
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

/* ------------------------------------------------------------------ plano */

static const int DC8[8] = { 1, 1, 0, -1, -1, -1,  0,  1 };   /* par = reto, impar = 45 */
static const int DL8[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };

static int plano_da_rede(const PbMapa *m, int rede)
{
    int i;
    for (i = 0; i < m->n_planos; i++)
        if (m->plano_rede[i] == rede && m->plano_face[i] == 1) return i;
    return -1;
}

static int acha(int *pai, int x)
{
    while (pai[x] != x) { pai[x] = pai[pai[x]]; x = pai[x]; }
    return x;
}

static void une(int *pai, int a, int b)
{
    a = acha(pai, a); b = acha(pai, b);
    if (a != b) pai[b] = a;
}

typedef struct {
    int *rotulo;     /* por celula: pedaco do plano, -1 = sem plano */
    int n_pedacos;
    int *pai;        /* pedacos 0..n-1 e pads n..n+r->n-1, unidos se ligam */
} Pedacos;

/* O plano e o cobre que sobra: celula dentro da zona e livre de qualquer cobre
 * alheio no mapa INFLADO. E conservador de proposito (o mapa inflado guarda
 * meia trilha + folga, mais que a folga da zona): o plano de verdade sai mais
 * largo, nunca mais estreito, do que a conta acha.
 *
 * Pedacos separados pelas trilhas sao rotulados (vizinhanca de 4: dois pedacos
 * que so se tocam na quina NAO se ligam no cobre). E cada pad da rede so conta
 * como ligado a um pedaco se o alivio termico fecha: andando do centro do pad
 * para fora, pelo menos DOIS raios (retos, ou a 45) encontram plano. Um raio so
 * e o que o DRC do KiCad chama de alivio termico faminto. */
static void pedacos_do_plano(PbMapa *m, int ip, const Rede *r, const PbAlvo *alvos,
                             Pedacos *pd)
{
    int cols = m->colunas, lins = m->linhas, face = m->plano_face[ip];
    int rede = m->plano_rede[ip];
    size_t n = (size_t)cols * lins, i;
    const unsigned char *regiao = m->plano_regiao[ip];
    int *fila = (int *)malloc(n * sizeof(int));
    int k;

    pd->rotulo = (int *)malloc(n * sizeof(int));
    pd->n_pedacos = 0;
    for (i = 0; i < n; i++) pd->rotulo[i] = -1;
    for (i = 0; i < n; i++) {
        int ini = 0, fim = 0;
        if (pd->rotulo[i] >= 0 || !regiao[i] || m->celulas[i * 2 + face] != PB_LIVRE)
            continue;
        fila[fim++] = (int)i;
        pd->rotulo[i] = pd->n_pedacos;
        while (ini < fim) {
            int cel = fila[ini++], c = cel % cols, l = cel / cols, d;
            for (d = 0; d < 8; d += 2) {
                int nc = c + DC8[d], nl = l + DL8[d], v;
                if (nc < 0 || nl < 0 || nc >= cols || nl >= lins) continue;
                v = nl * cols + nc;
                if (pd->rotulo[v] < 0 && regiao[v] && m->celulas[v * 2 + face] == PB_LIVRE) {
                    pd->rotulo[v] = pd->n_pedacos;
                    fila[fim++] = v;
                }
            }
        }
        pd->n_pedacos++;
    }
    free(fila);

    pd->pai = (int *)malloc((size_t)(pd->n_pedacos + r->n) * sizeof(int));
    for (k = 0; k < pd->n_pedacos + r->n; k++) pd->pai[k] = k;
    for (k = 0; k < r->n; k++) {
        const PbAlvo *a = &alvos[r->indices[k]];
        int bate[8], retos = 0, tortos = 0, d;
        for (d = 0; d < 8; d++) {
            int c = a->c, l = a->l, passos = 0;
            bate[d] = -1;
            while (passos < 200 && c >= 0 && l >= 0 && c < cols && l < lins
                   && m->celulas[(l * cols + c) * 2 + face] == rede) {
                c += DC8[d]; l += DL8[d]; passos++;
            }
            if (c >= 0 && l >= 0 && c < cols && l < lins)
                bate[d] = pd->rotulo[l * cols + c];
            if (bate[d] >= 0) { if (d % 2 == 0) retos++; else tortos++; }
        }
        for (d = 0; d < 8; d++)
            if (bate[d] >= 0 && ((d % 2 == 0 && retos >= 2) || (d % 2 == 1 && tortos >= 2)))
                une(pd->pai, pd->n_pedacos + k, bate[d]);
    }
}

/* Plano perdido: celulas da zona que nao viram plano ligado a rede. Um pedaco
 * de plano esta vivo se encosta (vizinhanca de 4) em cobre da propria rede - pad
 * ou trilha: a zona preenche ate ele, sem folga. Pedaco que nao encosta em nada
 * da rede o KiCad remove como ilha. */
double pb_plano_perdido(const PbMapa *m)
{
    int cols = m->colunas, lins = m->linhas, ip;
    size_t n = (size_t)cols * lins, i;
    long perdido = 0;
    int *fila, *rotulo;
    if (m->n_planos == 0) return 0.0;
    fila = (int *)malloc(n * sizeof(int));
    rotulo = (int *)malloc(n * sizeof(int));
    for (ip = 0; ip < m->n_planos; ip++) {
        const unsigned char *regiao = m->plano_regiao[ip];
        int face = m->plano_face[ip], rede = m->plano_rede[ip];
        long na_zona = 0, vivo = 0;
        if (face != 1) continue;
        for (i = 0; i < n; i++) { rotulo[i] = -1; if (regiao[i]) na_zona++; }
        for (i = 0; i < n; i++) {
            int ini = 0, fim = 0, encosta = 0;
            if (rotulo[i] >= 0 || !regiao[i] || m->celulas[i * 2 + face] != PB_LIVRE)
                continue;
            fila[fim++] = (int)i;
            rotulo[i] = 1;
            while (ini < fim) {
                int cel = fila[ini++], c = cel % cols, l = cel / cols, d;
                for (d = 0; d < 8; d += 2) {
                    int nc = c + DC8[d], nl = l + DL8[d], v, dono;
                    if (nc < 0 || nl < 0 || nc >= cols || nl >= lins) continue;
                    v = nl * cols + nc;
                    dono = m->celulas[v * 2 + face];
                    if (dono == rede) encosta = 1;
                    if (rotulo[v] < 0 && regiao[v] && dono == PB_LIVRE) {
                        rotulo[v] = 1;
                        fila[fim++] = v;
                    }
                }
            }
            if (encosta) vivo += fim;
        }
        perdido += na_zona - vivo;
    }
    free(fila);
    free(rotulo);
    return (double)perdido * m->regras.grade * m->regras.grade;
}

/* Junta ao que ja esta ligado o grupo `g` inteiro: os pads dele e todo o plano
 * dele viram ponto de partida. Devolve quantos pads entraram agora. */
static int junta_grupo(const PbMapa *m, int ip, Pedacos *pd, int g, const Rede *r,
                       const PbAlvo *alvos, char *ligado,
                       int *origens, int *n_origens, int max_origens)
{
    int k, entraram = 0, face = m->plano_face[ip];
    size_t i, n = (size_t)m->colunas * m->linhas;
    for (k = 0; k < r->n; k++) {
        int idx = r->indices[k];
        if (ligado[idx] || acha(pd->pai, pd->n_pedacos + k) != g) continue;
        ligado[idx] = 1;
        entraram++;
        if (*n_origens < max_origens) {
            origens[*n_origens * 3 + 0] = alvos[idx].c;
            origens[*n_origens * 3 + 1] = alvos[idx].l;
            origens[*n_origens * 3 + 2] = 1;
            (*n_origens)++;
        }
    }
    for (i = 0; i < n && *n_origens < max_origens; i++) {
        if (pd->rotulo[i] < 0 || acha(pd->pai, pd->rotulo[i]) != g) continue;
        pd->rotulo[i] = -1;                    /* ja entrou: nao entra de novo */
        origens[*n_origens * 3 + 0] = (int)(i % (size_t)m->colunas);
        origens[*n_origens * 3 + 1] = (int)(i / (size_t)m->colunas);
        origens[*n_origens * 3 + 2] = face;
        (*n_origens)++;
    }
    return entraram;
}

static int grupo_do_pad(Pedacos *pd, const Rede *r, int idx)
{
    int k;
    for (k = 0; k < r->n; k++)
        if (r->indices[k] == idx) return acha(pd->pai, pd->n_pedacos + k);
    return -1;
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
                       int teto, char *falhou)
{
    int i, j, ligadas = 0, n_origens;
    int n_segs = 0, n_vias = 0, n_abertas = 0;
    int *pares = (int *)malloc((size_t)(n_alvos > 0 ? n_alvos : 1) * 2 * sizeof(int));
    int n_pares = 0;

    /* Pad de plano que ficou sem ligacao numa passada anterior vai PRIMEIRO, com
     * trilha ate o pad da mesma rede mais perto. Rede de plano roteia por ultimo,
     * e as outras redes cercavam o pad antes de o plano chegar nele: sem passagem
     * nem para a trilha de costura. Isto e o rip-up da rede de plano. */
    for (j = 0; j < n_redes; j++) {
        Rede *r = &redes[j];
        if (plano_da_rede(m, r->rede) < 0) continue;
        for (i = 0; i < r->n; i++) {
            int a = r->indices[i], b = -1, k, n, orig[3], dest[3];
            double melhor_d = 1e30;
            if (!falhou[a]) continue;
            for (k = 0; k < r->n; k++) {
                int x = r->indices[k];
                double dc, dl, d;
                if (x == a) continue;
                dc = alvos[x].c - alvos[a].c; dl = alvos[x].l - alvos[a].l;
                d = dc * dc + dl * dl;
                if (d < melhor_d) { melhor_d = d; b = x; }
            }
            if (b < 0) continue;
            orig[0] = alvos[a].c; orig[1] = alvos[a].l; orig[2] = 1;
            dest[0] = alvos[b].c; dest[1] = alvos[b].l; dest[2] = 1;
            n = pb_roteia(m, r->rede, orig, 1, dest, 1, caminho, max_caminho * 4);
            if (n < 0) continue;            /* fica para a fase do plano */
            {
                int s0 = n_segs, v0 = n_vias;
                em_segmentos(caminho, n, r->rede, segs, &n_segs, max_segs,
                             vias, &n_vias, max_vias);
                pinta_trecho(m, segs, s0, n_segs, vias, v0, n_vias);
            }
            pares[n_pares * 2] = a;
            pares[n_pares * 2 + 1] = b;
            n_pares++;
        }
    }

    for (j = 0; j < n_redes; j++) {
        Rede *r = &redes[j];
        int restantes, ip = plano_da_rede(m, r->rede);
        Pedacos pd;
        if (r->n < 2) continue;

        memset(ligado, 0, (size_t)n_alvos);
        if (ip >= 0) {
            /* rede de plano: comeca do maior grupo (pads que o plano ja liga),
             * com o plano inteiro dele como ponto de partida */
            int k, g0 = -1, mais = 0, entraram;
            pedacos_do_plano(m, ip, r, alvos, &pd);
            /* pads que a trilha de costura ja juntou sao um grupo so */
            for (k = 0; k < n_pares; k++) {
                int q, ka = -1, kb = -1;
                for (q = 0; q < r->n; q++) {
                    if (r->indices[q] == pares[k * 2]) ka = q;
                    if (r->indices[q] == pares[k * 2 + 1]) kb = q;
                }
                if (ka >= 0 && kb >= 0)
                    une(pd.pai, pd.n_pedacos + ka, pd.n_pedacos + kb);
            }
            for (k = 0; k < r->n; k++) {
                int g = acha(pd.pai, pd.n_pedacos + k), q, conta = 0;
                for (q = 0; q < r->n; q++)
                    if (acha(pd.pai, pd.n_pedacos + q) == g) conta++;
                if (conta > mais) { mais = conta; g0 = g; }
            }
            n_origens = 0;
            entraram = junta_grupo(m, ip, &pd, g0, r, alvos, ligado,
                                   origens, &n_origens, max_caminho);
            restantes = r->n - entraram;
            ligadas += entraram - 1;
        } else {
            ligado[r->indices[0]] = 1;
            n_origens = 1;
            origens[0] = alvos[r->indices[0]].c;
            origens[1] = alvos[r->indices[0]].l;
            origens[2] = 1;                 /* chegada sempre pela face de baixo */
            restantes = r->n - 1;
        }

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
                if (ip >= 0) {
                    falhou[melhor] = 1;     /* na proxima passada, vai primeiro */
                    /* o grupo dele inteiro fica de fora: uma ligacao aberta */
                    int g = grupo_do_pad(&pd, r, melhor), k;
                    for (k = 0; k < r->n; k++)
                        if (!ligado[r->indices[k]]
                                && acha(pd.pai, pd.n_pedacos + k) == g) {
                            ligado[r->indices[k]] = 1;
                            restantes--;
                        }
                } else {
                    ligado[melhor] = 1;
                    restantes--;
                }
                /* ja perdeu para uma passada anterior: nao adianta terminar */
                if (teto >= 0 && n_abertas > teto) {
                    if (ip >= 0) { free(pd.rotulo); free(pd.pai); }
                    free(pares);
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
            if (ip >= 0) {
                /* chegou no pad: o grupo dele (pads e plano) entra junto */
                int g = grupo_do_pad(&pd, r, melhor), entraram;
                entraram = junta_grupo(m, ip, &pd, g, r, alvos, ligado,
                                       origens, &n_origens, max_caminho);
                restantes -= entraram;
                ligadas += entraram;
            } else {
                ligado[melhor] = 1;
                restantes--;
                ligadas++;
            }
        }
        if (ip >= 0) { free(pd.rotulo); free(pd.pai); }
    }
    free(pares);

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
    char *ligado = NULL, *falhou = NULL;
    int *base = NULL;
    size_t n_celulas = (size_t)m->colunas * m->linhas * 2;
    PbSeg *t_segs = NULL;
    PbViaOut *t_vias = NULL;
    PbAberta *t_abertas = NULL;
    int melhor_lig = 0, melhor_ns = 0, melhor_nv = 0, melhor_na = 0;
    double melhor_fio = 0.0, melhor_perda = 0.0;
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
    falhou = (char *)calloc((size_t)n_alvos, 1);

    passadas = m->regras.tentativas > 0 ? m->regras.tentativas : 1;
    for (passada = 0; passada < passadas; passada++) {
        int ns, nv, na, lig;
        double fio, perda;

        /* ordem: quem falhou vai na frente; o resto, nas duas primeiras passadas,
         * por numero de pads (mais pads primeiro); da terceira em diante, sorteado */
        for (j = 0; j < n_redes; j++)
            redes[j].plano = plano_da_rede(m, redes[j].rede) >= 0;
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
                          melhor_passada < 0 ? -1 : melhor_na, falhou);
        ultima = passada;
        fio = fio_de(m, t_segs, ns);
        perda = pb_plano_perdido(m);
        /* fecha mais > menos plano perdido > menos via > menos fio */
        if (melhor_passada < 0 || na < melhor_na
                || (na == melhor_na && perda < melhor_perda - 0.5)
                || (na == melhor_na && fabs(perda - melhor_perda) <= 0.5
                    && (nv < melhor_nv
                        || (nv == melhor_nv && fio < melhor_fio - 1e-6)))) {
            melhor_perda = perda;
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
    free(t_segs); free(t_vias); free(t_abertas); free(falhou);

    *n_segs_out = melhor_ns;
    *n_vias_out = melhor_nv;
    *n_abertas_out = melhor_na;
    return melhor_lig;
}
