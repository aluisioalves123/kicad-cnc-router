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
            {
                int k, ja = 0;
                for (k = 0; k < *n_vias; k++)
                    if (vias[k].c == an[0] && vias[k].l == an[1] && vias[k].rede == rede) {
                        ja = 1;          /* passou por uma via que ja existe */
                        break;
                    }
                if (!ja && *n_vias < max_vias) {
                    PbViaOut *v = &vias[(*n_vias)++];
                    v->c = an[0]; v->l = an[1]; v->rede = rede;
                }
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
        pb_anota_via(m, vias[i].c, vias[i].l, vias[i].rede);
    }
    (void)folga;
}

/* ------------------------------------------------------------------ pad */

/* Celulas do cobre do pad onde a PONTA da trilha cabe (livres para a rede no
 * mapa inflado), como triplas (coluna, linha, face de baixo). E por elas que a
 * trilha chega - ou sai, no primeiro pad. Sem forma (larg = 0), so o centro. */
static int celulas_do_pad(const PbMapa *m, const PbAlvo *a, int rede,
                          int *saida, int max)
{
    double hx = a->larg / 2.0, hy = a->alt / 2.0, raio = hx > hy ? hx : hy;
    double t = a->giro * M_PI / 180.0, ct = cos(t), st = sin(t);   /* mundo -> local */
    int c0 = pb_coluna_de(m, a->x - raio), c1 = pb_coluna_de(m, a->x + raio);
    int l0 = pb_linha_de(m, a->y - raio), l1 = pb_linha_de(m, a->y + raio);
    int c, l, n = 0;
    if (a->larg <= 0.0 || a->alt <= 0.0) {
        saida[0] = a->c; saida[1] = a->l; saida[2] = a->face;
        return 1;
    }
    for (l = l0; l <= l1; l++)
        for (c = c0; c <= c1 && n < max; c++) {
            double dx = pb_x_de(m, c) - a->x, dy = pb_y_de(m, l) - a->y;
            double lx = dx * ct - dy * st, ly = dx * st + dy * ct;
            int dentro;
            if (a->forma == PB_CIRCULO) {
                dentro = lx * lx + ly * ly <= hx * hx;
            } else if (a->forma == PB_CAPSULA) {
                double r = hx < hy ? hx : hy, ex = hx - r, ey = hy - r;
                double px = fabs(lx) - ex, py = fabs(ly) - ey;
                if (px < 0) px = 0;
                if (py < 0) py = 0;
                dentro = px * px + py * py <= r * r;
            } else {
                dentro = fabs(lx) <= hx && fabs(ly) <= hy;
            }
            if (!dentro || !pb_livre_para(m, c, l, a->face, rede)) continue;
            saida[n * 3] = c; saida[n * 3 + 1] = l; saida[n * 3 + 2] = a->face;
            n++;
        }
    if (n == 0) {               /* nada cabe: o centro, e o A* responde que nao da */
        saida[0] = a->c; saida[1] = a->l; saida[2] = a->face;
        n = 1;
    }
    return n;
}

#define MAX_CEL_PAD 20000

/* ------------------------------------------------------------------ plano */

#define RAIZ2_P 1.41421356237309504880
static const int DC8[8] = { 1, 1, 0, -1, -1, -1,  0,  1 };   /* par = reto, impar = 45 */
static const int DL8[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };

static int plano_da_rede(const PbMapa *m, int rede)
{
    int i;
    for (i = 0; i < m->n_planos; i++)
        if (m->plano_rede[i] == rede) return i;
    return -1;
}

/* A celula, nesta face, e plano de verdade de ALGUMA zona da rede? (Duas zonas
 * da mesma rede na mesma face sao um plano so onde se tocam.) */
static int e_plano_da_rede(const PbMapa *m, int rede, int face, int cel)
{
    int ip;
    for (ip = 0; ip < m->n_planos; ip++)
        if (m->plano_rede[ip] == rede && m->plano_face[ip] == face && pb_e_plano(m, ip, cel))
            return 1;
    return 0;
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

/* Quanto o pad avanca na direcao `ang` (radianos, mundo, Y para baixo). */
static double meio_eixo(const PbAlvo *a, double ang)
{
    double hx = a->larg / 2.0, hy = a->alt / 2.0;
    double t = ang + a->giro * M_PI / 180.0;           /* mundo -> local */
    double ct = fabs(cos(t)), st = fabs(sin(t));
    if (a->forma == PB_CIRCULO) return hx;
    if (a->forma == PB_CAPSULA) {
        double r = hx < hy ? hx : hy;
        return hx >= hy ? ct * (hx - r) + r : st * (hy - r) + r;
    }
    {
        double rx = ct > 1e-12 ? hx / ct : 1e30, ry = st > 1e-12 ? hy / st : 1e30;
        return rx < ry ? rx : ry;
    }
}

typedef struct {
    int *rotulo;     /* por celula: pedaco do plano, -1 = sem plano */
    int n_pedacos;
    int *pai;        /* pedacos 0..n-1 e pads n..n+r->n-1, unidos se ligam */
} Pedacos;

/* Ha plano preenchido no ponto (x, y) mm? O nucleo erodido (pb_e_plano) sao
 * centros de disco de espessura/2 que cabem; o preenchimento cobre ate essa
 * distancia deles, e um pouco mais ate a folga. Devolve o pedaco, ou -1. */
static int plano_perto(const PbMapa *m, int ip, const Pedacos *pd, double x, double y)
{
    double g = m->regras.grade;
    double nucleo = pb_inflacao(m) + m->plano_erosao[ip] * g;   /* do nucleo ao alheio */
    double raio = nucleo - m->plano_folga[ip];
    int c0, l0, r, dc, dl, cols = m->colunas, lins = m->linhas;
    if (m->plano_espessura[ip] / 2.0 < raio) raio = m->plano_espessura[ip] / 2.0;
    raio -= g * 0.75;                     /* a grade nao pode dar o que nao tem */
    if (raio < 0) raio = 0;
    c0 = pb_coluna_de(m, x); l0 = pb_linha_de(m, y);
    r = (int)ceil(raio / g);
    for (dl = -r; dl <= r; dl++)
        for (dc = -r; dc <= r; dc++) {
            int c = c0 + dc, l = l0 + dl, rot;
            double ex = pb_x_de(m, c) - x, ey = pb_y_de(m, l) - y;
            if (c < 0 || l < 0 || c >= cols || l >= lins) continue;
            if (ex * ex + ey * ey > raio * raio) continue;
            rot = pd->rotulo[(l * cols + c) * 2 + m->plano_face[ip]];
            if (rot >= 0) return rot;
        }
    return -1;
}

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
static void pedacos_do_plano(PbMapa *m, int rede, const Rede *r, const PbAlvo *alvos,
                             Pedacos *pd)
{
    int cols = m->colunas, lins = m->linhas, face, ip;
    size_t n = (size_t)cols * lins, i;
    int *fila = (int *)malloc(n * sizeof(int));
    int k;

    /* rotulo por celula x FACE: a rede pode ter plano embaixo e em cima, e os
     * pedacos das duas faces entram no mesmo agrupamento - quem os junta e via
     * de costura, que o roteamento da rede faz quando precisa */
    pd->rotulo = (int *)malloc(n * 2 * sizeof(int));
    pd->n_pedacos = 0;
    for (i = 0; i < n * 2; i++) pd->rotulo[i] = -1;
    for (face = 0; face < 2; face++)
        for (i = 0; i < n; i++) {
            int ini = 0, fim = 0;
            if (pd->rotulo[i * 2 + face] >= 0 || !e_plano_da_rede(m, rede, face, (int)i))
                continue;
            fila[fim++] = (int)i;
            pd->rotulo[i * 2 + face] = pd->n_pedacos;
            while (ini < fim) {
                int cel = fila[ini++], c = cel % cols, l = cel / cols, d;
                for (d = 0; d < 8; d += 2) {
                    int nc = c + DC8[d], nl = l + DL8[d], v;
                    if (nc < 0 || nl < 0 || nc >= cols || nl >= lins) continue;
                    v = nl * cols + nc;
                    if (pd->rotulo[v * 2 + face] < 0 && e_plano_da_rede(m, rede, face, v)) {
                        pd->rotulo[v * 2 + face] = pd->n_pedacos;
                        fila[fim++] = v;
                    }
                }
            }
            pd->n_pedacos++;
        }
    free(fila);

    pd->pai = (int *)malloc((size_t)(pd->n_pedacos + r->n) * sizeof(int));
    for (k = 0; k < pd->n_pedacos + r->n; k++) pd->pai[k] = k;
    for (k = 0; k < r->n; k++)
    for (ip = 0; ip < m->n_planos; ip++) {
        const PbAlvo *a = &alvos[r->indices[k]];
        int bate[8], retos = 0, tortos = 0, d, usa_retos = 1, usa_tortos = 1;
        if (m->plano_rede[ip] != rede || !m->plano_liga[ip]) continue;
        /* Os raios saem em angulos FIXOS, e com largura: o KiCad acusou alivio
         * termico faminto (C4 no interfone) quando a conta aceitava quaisquer 2
         * de 8 direcoes testando um fio so. */
        /* pad que solda do OUTRO lado (peca virada) nao liga neste plano:
         * seria solda do lado do corpo */
        if (a->face != m->plano_face[ip]) continue;
        if (a->raio_termico > -900.0) {
            double a90 = fmod(fmod(a->raio_termico, 90.0) + 90.0, 90.0);
            if (a90 < 5.0 || a90 > 85.0) usa_tortos = 0;
            else if (fabs(a90 - 45.0) < 5.0) usa_retos = 0;
        }
        for (d = 0; d < 8; d++) {
            /* O raio do KiCad e CURTO: vai do pad, atravessa o vao do alivio e
             * so conta se ali ja houver plano. Andar para fora ate achar plano,
             * como era antes, aceitava plano a 1,3 mm - onde o raio nao chega -
             * e o DRC acusava alivio faminto (C4, Q1 no interfone). */
            double ux = DC8[d], uy = DL8[d], nrm = sqrt(ux * ux + uy * uy);
            double alcance = meio_eixo(a, atan2(uy, ux)) + m->plano_alivio[ip] + 0.05;
            double meia = m->plano_raio[ip] / 2.0 * 0.8, q;
            int peca = -1, ok = 1;
            bate[d] = -1;
            if ((d % 2 == 0 && !usa_retos) || (d % 2 == 1 && !usa_tortos)) continue;
            ux /= nrm; uy /= nrm;
            /* a ponta do raio, no centro e nas duas beiradas */
            for (q = -meia; q <= meia + 1e-9 && ok; q += (meia > 0 ? meia : 1.0)) {
                double px = a->x + ux * alcance - uy * q, py = a->y + uy * alcance + ux * q;
                int cai = plano_perto(m, ip, pd, px, py);
                if (cai < 0) ok = 0;
                else if (peca < 0) peca = cai;
                if (meia <= 0) break;
            }
            bate[d] = ok ? peca : -1;
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
        for (i = 0; i < n; i++) { rotulo[i] = -1; if (regiao[i]) na_zona++; }
        for (i = 0; i < n; i++) {
            int ini = 0, fim = 0, encosta = 0;
            if (rotulo[i] >= 0 || !pb_e_plano(m, ip, (int)i))
                continue;
            fila[fim++] = (int)i;
            rotulo[i] = 1;
            while (ini < fim) {
                int cel = fila[ini++], c = cel % cols, l = cel / cols, d;
                for (d = 0; d < 8; d += 2) {
                    int nc = c + DC8[d], nl = l + DL8[d], v;
                    if (nc < 0 || nl < 0 || nc >= cols || nl >= lins) continue;
                    v = nl * cols + nc;
                    if (rotulo[v] < 0 && pb_e_plano(m, ip, v)) {
                        rotulo[v] = 1;
                        fila[fim++] = v;
                    }
                }
                /* encosta na rede: cobre dela a ate a erosao + 1 celula */
                if (!encosta) {
                    int e = m->plano_erosao[ip] + 1, dc, dl;
                    for (dl = -e; dl <= e && !encosta; dl++)
                        for (dc = -e; dc <= e; dc++) {
                            int nc = c + dc, nl = l + dl;
                            if (nc < 0 || nl < 0 || nc >= cols || nl >= lins) continue;
                            if (m->celulas[(nl * cols + nc) * 2 + face] == rede) {
                                encosta = 1;
                                break;
                            }
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
static int junta_grupo(const PbMapa *m, Pedacos *pd, int g, const Rede *r,
                       const PbAlvo *alvos, char *ligado,
                       int *origens, int *n_origens, int max_origens)
{
    int k, entraram = 0;
    size_t i, n = (size_t)m->colunas * m->linhas * 2;
    for (k = 0; k < r->n; k++) {
        int idx = r->indices[k];
        if (ligado[idx] || acha(pd->pai, pd->n_pedacos + k) != g) continue;
        ligado[idx] = 1;
        entraram++;
        if (*n_origens < max_origens) {
            origens[*n_origens * 3 + 0] = alvos[idx].c;
            origens[*n_origens * 3 + 1] = alvos[idx].l;
            origens[*n_origens * 3 + 2] = alvos[idx].face;
            (*n_origens)++;
        }
    }
    for (i = 0; i < n && *n_origens < max_origens; i++) {
        if (pd->rotulo[i] < 0 || acha(pd->pai, pd->rotulo[i]) != g) continue;
        pd->rotulo[i] = -1;                    /* ja entrou: nao entra de novo */
        origens[*n_origens * 3 + 0] = (int)((i / 2) % (size_t)m->colunas);
        origens[*n_origens * 3 + 1] = (int)((i / 2) / (size_t)m->colunas);
        origens[*n_origens * 3 + 2] = (int)(i % 2);
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

/* ------------------------------------------------------------------ rasgo */

/* Custo por celula de atravessar cobre alheio, e quantas vezes cada rede pode
 * ser rasgada numa passada. O custo cresce a cada passada, como no Freerouting
 * (custo inicial x numero da passada): no comeco rasga facil, depois fica caro e
 * a solucao assenta. O limite garante que a passada termina. */
#define LIMITE_RASGOS    2

static double dist_ponto_seg(double px, double py, double ax, double ay,
                             double bx, double by)
{
    double dx = bx - ax, dy = by - ay, t, qx, qy, l2 = dx * dx + dy * dy;
    t = l2 > 1e-12 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    qx = ax + t * dx; qy = ay + t * dy;
    return sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

static double dist_seg_seg(double ax, double ay, double bx, double by,
                           double cx, double cy, double dx, double dy)
{
    double d1 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    double d2 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
    double d3 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
    double d4 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
    double m, t;
    if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) return 0.0;   /* cruzam */
    m = dist_ponto_seg(ax, ay, cx, cy, dx, dy);
    t = dist_ponto_seg(bx, by, cx, cy, dx, dy); if (t < m) m = t;
    t = dist_ponto_seg(cx, cy, ax, ay, bx, by); if (t < m) m = t;
    t = dist_ponto_seg(dx, dy, ax, ay, bx, by); if (t < m) m = t;
    return m;
}

/* Redes alheias que o cobre novo [s0, ns) / [v0, nv) atropela, medido em mm. */
static int atropeladas(const PbMapa *m, const PbSeg *segs, int s0, int ns,
                       const PbViaOut *vias, int v0, int nv, int rede,
                       int *saida, int max)
{
    double t = m->regras.trilha, f = m->regras.folga, v = m->regras.via_ilha;
    int i, k, n = 0;
#define X(c) pb_x_de(m, c)
#define Y(l) pb_y_de(m, l)
#define ANOTA(r) do { int q_, ja_ = 0; for (q_ = 0; q_ < n; q_++) if (saida[q_] == (r)) ja_ = 1; \
                      if (!ja_ && n < max) saida[n++] = (r); } while (0)
    for (i = 0; i < s0; i++) {
        const PbSeg *a = &segs[i];
        if (a->rede == rede || a->face < 0) continue;
        for (k = s0; k < ns; k++) {
            const PbSeg *b = &segs[k];
            if (a->face != b->face) continue;
            if (dist_seg_seg(X(a->c1), Y(a->l1), X(a->c2), Y(a->l2),
                             X(b->c1), Y(b->l1), X(b->c2), Y(b->l2)) < t + f - 1e-6) {
                ANOTA(a->rede); break;
            }
        }
        for (k = v0; k < nv; k++)
            if (dist_ponto_seg(X(vias[k].c), Y(vias[k].l), X(a->c1), Y(a->l1),
                               X(a->c2), Y(a->l2)) < t / 2 + v / 2 + f - 1e-6) {
                ANOTA(a->rede); break;
            }
    }
    for (i = 0; i < v0; i++) {
        if (vias[i].rede == rede) continue;
        for (k = s0; k < ns; k++)
            if (dist_ponto_seg(X(vias[i].c), Y(vias[i].l), X(segs[k].c1), Y(segs[k].l1),
                               X(segs[k].c2), Y(segs[k].l2)) < t / 2 + v / 2 + f - 1e-6) {
                ANOTA(vias[i].rede); break;
            }
        for (k = v0; k < nv; k++) {
            double dx = X(vias[i].c) - X(vias[k].c), dy = Y(vias[i].l) - Y(vias[k].l);
            if (sqrt(dx * dx + dy * dy) < v + f - 1e-6) { ANOTA(vias[i].rede); break; }
        }
    }
#undef X
#undef Y
#undef ANOTA
    return n;
}

/* Tira do resultado todo o cobre de uma rede (e as ligacoes abertas dela, que
 * vao ser tentadas de novo). */
static void tira_rede(int rede, PbSeg *segs, int *ns, PbViaOut *vias, int *nv,
                      PbAberta *abertas, int *na)
{
    int i, k;
    for (i = k = 0; i < *ns; i++) if (segs[i].rede != rede) segs[k++] = segs[i];
    *ns = k;
    for (i = k = 0; i < *nv; i++) if (vias[i].rede != rede) vias[k++] = vias[i];
    *nv = k;
    for (i = k = 0; i < *na; i++) if (abertas[i].rede != rede) abertas[k++] = abertas[i];
    *na = k;
}

/* Mapa de volta ao de antes das trilhas, e todo o cobre que sobrou pintado. */
static void reconstroi(PbMapa *m, const int *base, size_t n_celulas, int n_via_base,
                       const PbSeg *segs, int ns, const PbViaOut *vias, int nv)
{
    memcpy(m->celulas, base, n_celulas * sizeof(int));
    m->soma_da_rede = -999999;
    m->n_via_lista = n_via_base;
    pinta_trecho(m, segs, 0, ns, vias, 0, nv);
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
                       int teto, char *falhou,
                       const int *base, size_t n_celulas, int n_via_base, int passada)
{
    int i, j, ligadas = 0, n_origens;
    int n_segs = 0, n_vias = 0, n_abertas = 0;
    int *fila, *rasgos, *lig_rede, cab = 0, cauda = 0, rasgos_total = 0;
    int atrop[64];
    int *pares = (int *)malloc((size_t)(n_alvos > 0 ? n_alvos : 1) * 2 * sizeof(int));
    int n_pares = 0;
    int *celpad = (int *)malloc((size_t)MAX_CEL_PAD * 3 * sizeof(int));
    int *celpad2 = (int *)malloc((size_t)MAX_CEL_PAD * 3 * sizeof(int));

    /* Pad de plano que ficou sem ligacao numa passada anterior vai PRIMEIRO, com
     * trilha ate o pad da mesma rede mais perto. Rede de plano roteia por ultimo,
     * e as outras redes cercavam o pad antes de o plano chegar nele: sem passagem
     * nem para a trilha de costura. Isto e o rip-up da rede de plano. */
    for (j = 0; j < n_redes; j++) {
        Rede *r = &redes[j];
        if (plano_da_rede(m, r->rede) < 0) continue;
        for (i = 0; i < r->n; i++) {
            int a = r->indices[i], b = -1, k, n, no, nd;
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
            no = celulas_do_pad(m, &alvos[a], r->rede, celpad, MAX_CEL_PAD);
            nd = celulas_do_pad(m, &alvos[b], r->rede, celpad2, MAX_CEL_PAD);
            n = pb_roteia(m, r->rede, celpad, no, celpad2, nd, caminho, max_caminho * 4);
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

    /* Fila de redes: a rede rasgada volta para o fim dela. */
    fila = (int *)malloc((size_t)(n_redes * (LIMITE_RASGOS + 1) + 1) * sizeof(int));
    rasgos = (int *)calloc((size_t)(n_redes > 0 ? n_redes : 1), sizeof(int));
    lig_rede = (int *)calloc((size_t)(n_redes > 0 ? n_redes : 1), sizeof(int));
    for (j = 0; j < n_redes; j++) fila[cauda++] = j;
    while (cab < cauda) {
        Rede *r;
        int restantes, ip;
        j = fila[cab++];
        r = &redes[j];
        ip = plano_da_rede(m, r->rede);
        if (m->parar && *m->parar) break;   /* quem chamou desistiu */
        Pedacos pd;
        if (r->n < 2) continue;

        memset(ligado, 0, (size_t)n_alvos);
        if (ip >= 0) {
            /* rede de plano: comeca do maior grupo (pads que o plano ja liga),
             * com o plano inteiro dele como ponto de partida */
            int k, g0 = -1, mais = 0, entraram;
            pedacos_do_plano(m, r->rede, r, alvos, &pd);
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
            entraram = junta_grupo(m, &pd, g0, r, alvos, ligado,
                                   origens, &n_origens, max_caminho);
            restantes = r->n - entraram;
            lig_rede[j] += entraram - 1;
        } else {
            ligado[r->indices[0]] = 1;
            /* parte de qualquer ponto do primeiro pad onde a trilha caiba */
            n_origens = celulas_do_pad(m, &alvos[r->indices[0]], r->rede, origens,
                                       max_caminho);
            restantes = r->n - 1;
        }

        while (restantes > 0) {
            int melhor = -1, n, n_destino, rasgou, q;
            double melhor_d = 1e30;

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

            n_destino = celulas_do_pad(m, &alvos[melhor], r->rede, celpad, MAX_CEL_PAD);
            n = pb_roteia(m, r->rede, origens, n_origens, celpad, n_destino,
                          caminho, max_caminho * 4);
            rasgou = 0;
            if (n < 0 && ip < 0 && m->regras.rasgo > 0.0 && rasgos_total < 2 * n_redes) {
                /* nao passou: tenta de novo podendo atravessar cobre alheio */
                int s0 = n_segs, v0 = n_vias, na_, ok = 1;
                m->rasgo = m->regras.rasgo * (passada + 1);
                m->rasgo_base = base;
                n = pb_roteia(m, r->rede, origens, n_origens, celpad, n_destino,
                              caminho, max_caminho * 4);
                m->rasgo = 0.0;
                if (n >= 0) {
                    em_segmentos(caminho, n, r->rede, segs, &n_segs, max_segs,
                                 vias, &n_vias, max_vias);
                    na_ = atropeladas(m, segs, s0, n_segs, vias, v0, n_vias, r->rede,
                                      atrop, 64);
                    for (q = 0; q < na_ && ok; q++) {
                        int jx;
                        for (jx = 0; jx < n_redes; jx++) if (redes[jx].rede == atrop[q]) break;
                        if (jx == n_redes || plano_da_rede(m, atrop[q]) >= 0
                                || rasgos[jx] >= LIMITE_RASGOS)
                            ok = 0;
                    }
                    if (!ok || na_ == 0) {
                        /* nao vale (ou nao atropelou nada: e outra coisa) */
                        n_segs = s0; n_vias = v0; n = -1;
                    } else {
                        for (q = 0; q < na_; q++) {
                            int jx;
                            for (jx = 0; jx < n_redes; jx++) if (redes[jx].rede == atrop[q]) break;
                            rasgos[jx]++;
                            rasgos_total++;
                            tira_rede(atrop[q], segs, &n_segs, vias, &n_vias,
                                      abertas, &n_abertas);
                            lig_rede[jx] = 0;
                            fila[cauda++] = jx;
                        }
                        reconstroi(m, base, n_celulas, n_via_base, segs, n_segs,
                                   vias, n_vias);
                        rasgou = 1;
                    }
                }
            }
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
                    for (q = 0; q < n_redes; q++) ligadas += lig_rede[q];
                    free(pares); free(celpad); free(celpad2);
                    free(fila); free(rasgos); free(lig_rede);
                    *n_segs_out = n_segs;
                    *n_vias_out = n_vias;
                    *n_abertas_out = n_abertas;
                    return ligadas;
                }
                continue;
            }

            if (!rasgou) {                  /* com rasgo, ja entrou no mapa refeito */
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
                entraram = junta_grupo(m, &pd, g, r, alvos, ligado,
                                       origens, &n_origens, max_caminho);
                restantes -= entraram;
                lig_rede[j] += entraram;
            } else {
                ligado[melhor] = 1;
                restantes--;
                lig_rede[j]++;
            }
        }
        if (ip >= 0) { free(pd.rotulo); free(pd.pai); }
    }
    for (j = 0; j < n_redes; j++) ligadas += lig_rede[j];
    free(pares); free(celpad); free(celpad2);
    free(fila); free(rasgos); free(lig_rede);

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
    int n_via_base = 0;
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
    n_via_base = m->n_via_lista;
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
            m->n_via_lista = n_via_base;
            m->soma_da_rede = -999999;
        }
        lig = uma_passada(m, alvos, n_alvos, redes, n_redes,
                          t_segs, max_segs, t_vias, max_vias, t_abertas, max_abertas,
                          &ns, &nv, &na, caminho, max_caminho, origens, ligado,
                          melhor_passada < 0 ? -1 : melhor_na, falhou,
                          base, n_celulas, n_via_base, passada);
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
        if (m->parar && *m->parar) break;
    }

    /* o mapa termina com o cobre da passada que ganhou, nao com o da ultima */
    if (ultima != melhor_passada) {
        memcpy(m->celulas, base, n_celulas * sizeof(int));
            m->n_via_lista = n_via_base;
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
