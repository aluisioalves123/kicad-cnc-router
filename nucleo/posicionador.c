/* Posicionamento com o ROTEADOR como juiz.
 *
 * O custo geometrico (comprimento + espalhamento + proibicoes) e bom para gerar
 * candidatos e ruim para escolher entre eles: medido na blink blink, o candidato
 * mais barato fechou 12 ligacoes a menos que um mais caro. Entao a divisao e:
 *
 *   recozimento no custo geometrico  ->  gera K candidatos diferentes
 *   roteamento completo de cada um   ->  quem fechar mais, ganha
 *
 * Isso so e possivel porque rotear custa 0,2 s. A 14 s por roteacao, que era o
 * Python, este laco levaria oito minutos.
 *
 * O candidato ZERO e a posicao de entrada, intocada. O resultado nunca pode sair
 * pior do que quem chamou ja tinha - na pior hipotese, nada muda.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "perfcnc.h"
#include "interno.h"

/* CONVENCAO DE GIRO DO KICAD: Y aponta para baixo, entao giro positivo na tela e
 * NEGATIVO na matematica. mundo = Rot(-giro) . local; local = Rot(+giro) . mundo.
 * Errei o sinal na ida e na volta; o teste de ida-e-volta nao tinha como ver. */
#define W_SOBREPOE  500.0
#define W_FORA      800.0
#define W_PROIBIDA  800.0
#define W_ESPALHA   0.9
#define W_PAD       2000.0    /* por mm de folga faltando entre pads de pecas diferentes */
/* Zero. O corpo agora e o COURTYARD, e o DRC do KiCad mostra zero courtyard
 * sobreposto na placa feita a mao. A tolerancia de 1 mm2 que existia aqui era
 * compensacao por medir com a caixa errada, e deixou passar R3 cruzando D1. */
#define TOLERA_SOBREPOSICAO 1e-6
#define REGIOES     3
#define GRADE_POS   0.25      /* mm: posicao com 7 casas decimais nao ajuda ninguem */

/* ------------------------------------------------------------------ sorteio */

static unsigned long long proximo(unsigned long long *s)
{
    unsigned long long x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

static double sorteio(unsigned long long *s)
{
    return (double)(proximo(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* ------------------------------------------------------------------ geometria */

/* O corpo NAO fica centrado na origem do footprint: num resistor ele esta 3,81 mm
 * deslocado, num TO-92 2,57 mm. Tratar como centrado jogou Q2 para fora da placa e
 * empilhou pecas enquanto o custo jurava que estava tudo certo. */
static void caixa(const PbPeca *p, double *x0, double *y0, double *x1, double *y1)
{
    double larg = p->larg, alt = p->alt;
    double a = -p->giro * M_PI / 180.0;          /* local -> mundo */
    double mx = p->x + p->cx * cos(a) - p->cy * sin(a);
    double my = p->y + p->cx * sin(a) + p->cy * cos(a);
    int g = ((int)floor(p->giro + 0.5)) % 180;
    if (g < 0) g += 180;
    if (g == 90) { double t = larg; larg = alt; alt = t; }
    *x0 = mx - larg / 2.0; *y0 = my - alt / 2.0;
    *x1 = mx + larg / 2.0; *y1 = my + alt / 2.0;
}

static void pos_do_pad(const PbPeca *pecas, const PbPadC *pad, double *x, double *y)
{
    const PbPeca *p = &pecas[pad->peca];
    double a = -p->giro * M_PI / 180.0, ca = cos(a), sa = sin(a);   /* local -> mundo */
    *x = p->x + pad->dx * ca - pad->dy * sa;
    *y = p->y + pad->dx * sa + pad->dy * ca;
}

/* ------------------------------------------------------------------ custo */

typedef struct {
    const PbRegras *r;
    double bx0, by0, bx1, by1;          /* caixa do contorno */
    const PbProibida *proibidas;
    int n_proibidas;
    PbPeca *pecas;
    int n_pecas;
    const PbPadC *pads;
    int n_pads;
    int *redes;                          /* redes distintas */
    int n_redes;
} Ctx;

/* Quanto o pad avanca na direcao `ang` (radianos, mundo). A pergunta nunca e "qual
 * o tamanho do pad", e sim quanto ele avanca NA DIRECAO do vizinho: num oval de
 * 1,8 x 3 virado para o pino ao lado, usar o eixo maior erra o vao em mais de 1 mm. */
static double raio_na_direcao(const PbPadC *pad, double giro_peca, double ang)
{
    double a = pad->larg / 2.0, b = pad->alt / 2.0;
    double t = ang + (giro_peca + pad->giro_local) * M_PI / 180.0;   /* mundo -> local */
    double ct = fabs(cos(t)), st = fabs(sin(t));
    if (pad->forma == PB_CIRCULO) return a;
    if (pad->forma == PB_CAPSULA) {
        double r = (a < b) ? a : b, meio = fabs(a - b);
        /* capsula: meio-comprimento reto mais a ponta redonda */
        if (a >= b) return ct * meio + r;
        return st * meio + r;
    }
    {
        double rx = ct > 1e-12 ? a / ct : 1e30, ry = st > 1e-12 ? b / st : 1e30;
        return rx < ry ? rx : ry;
    }
}

/* Folga faltando entre pads de PECAS DIFERENTES, somada em mm. E a regra eletrica
 * que importa - foi o que se viu na tela: pad de uma peca encostado no de outra. */
static double pads_colados(const Ctx *c)
{
    double total = 0.0;
    int i, k;
    for (i = 0; i < c->n_pads; i++) {
        const PbPadC *pa = &c->pads[i];
        double xa, ya;
        pos_do_pad(c->pecas, pa, &xa, &ya);
        for (k = i + 1; k < c->n_pads; k++) {
            const PbPadC *pb = &c->pads[k];
            double xb, yb, d, ang, falta;
            if (pa->peca == pb->peca) continue;
            if (!c->pecas[pa->peca].movel && !c->pecas[pb->peca].movel) continue;
            pos_do_pad(c->pecas, pb, &xb, &yb);
            d = hypot(xb - xa, yb - ya);
            ang = atan2(yb - ya, xb - xa);
            falta = raio_na_direcao(pa, c->pecas[pa->peca].giro, ang)
                  + raio_na_direcao(pb, c->pecas[pb->peca].giro, ang + M_PI)
                  + c->r->folga - d;
            /* mesma rede pode encostar o cobre, mas aqui sao pecas diferentes: o
               corpo de uma nao cabe em cima do pad da outra */
            if (falta > 0) total += falta;
        }
    }
    return total;
}

static double comprimento(const Ctx *c)
{
    double total = 0.0;
    int k, i;
    for (k = 0; k < c->n_redes; k++) {
        double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30;
        int n = 0;
        for (i = 0; i < c->n_pads; i++) {
            double x, y;
            if (c->pads[i].rede != c->redes[k]) continue;
            pos_do_pad(c->pecas, &c->pads[i], &x, &y);
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
            n++;
        }
        if (n >= 2) total += (mxx - mnx) + (mxy - mny);
    }
    return total;
}

static double espalhamento(const Ctx *c)
{
    double area[REGIOES * REGIOES], media = 0.0, soma = 0.0;
    double lx = c->bx1 - c->bx0, ly = c->by1 - c->by0;
    int i;
    memset(area, 0, sizeof(area));
    for (i = 0; i < c->n_pecas; i++) {
        double x0, y0, x1, y1, cx, cy;
        int col, lin;
        caixa(&c->pecas[i], &x0, &y0, &x1, &y1);
        cx = (x0 + x1) / 2.0; cy = (y0 + y1) / 2.0;
        col = (int)((cx - c->bx0) * REGIOES / (lx > 1e-9 ? lx : 1e-9));
        lin = (int)((cy - c->by0) * REGIOES / (ly > 1e-9 ? ly : 1e-9));
        if (col < 0) col = 0;
        if (col >= REGIOES) col = REGIOES - 1;
        if (lin < 0) lin = 0;
        if (lin >= REGIOES) lin = REGIOES - 1;
        area[lin * REGIOES + col] += (x1 - x0) * (y1 - y0);
    }
    for (i = 0; i < REGIOES * REGIOES; i++) media += area[i];
    media /= REGIOES * REGIOES;
    for (i = 0; i < REGIOES * REGIOES; i++) soma += (area[i] - media) * (area[i] - media);
    return W_ESPALHA * soma / (REGIOES * REGIOES);
}

/* Corpo contra corpo, SEM folga de cobre: a folga vale entre trilhas de redes
 * diferentes, nao entre pecas. E so par em que ao menos uma pode se mexer -
 * penalidade sobre peca presa e constante que o otimizador nao melhora, e
 * constante grande distorce a temperatura, que sai do proprio custo. */
static double sobreposicao(const Ctx *c)
{
    double total = 0.0;
    int i, k;
    for (i = 0; i < c->n_pecas; i++) {
        double a0, b0, a1, b1;
        caixa(&c->pecas[i], &a0, &b0, &a1, &b1);
        for (k = i + 1; k < c->n_pecas; k++) {
            double c0, d0, c1, d1, dx, dy;
            if (!c->pecas[i].movel && !c->pecas[k].movel) continue;
            caixa(&c->pecas[k], &c0, &d0, &c1, &d1);
            dx = (a1 < c1 ? a1 : c1) - (a0 > c0 ? a0 : c0);
            dy = (b1 < d1 ? b1 : d1) - (b0 > d0 ? b0 : d0);
            if (dx > 0 && dy > 0 && dx * dy > TOLERA_SOBREPOSICAO)
                total += dx * dy;
        }
    }
    return total;
}

/* LIMITACAO CONHECIDA: contra o RETANGULO do contorno, nao o poligono. Numa
 * placa retangular da no mesmo; numa placa em L deixaria peca no recorte. */
static double fora(const Ctx *c)
{
    double total = 0.0;
    int i;
    for (i = 0; i < c->n_pecas; i++) {
        double x0, y0, x1, y1;
        if (!c->pecas[i].movel) continue;
        caixa(&c->pecas[i], &x0, &y0, &x1, &y1);
        if (c->bx0 > x0) total += c->bx0 - x0;
        if (x1 > c->bx1) total += x1 - c->bx1;
        if (c->by0 > y0) total += c->by0 - y0;
        if (y1 > c->by1) total += y1 - c->by1;
    }
    return total;
}

/* Area EXATA do poligono dentro do retangulo: recorta o poligono pelos quatro
 * lados (Sutherland-Hodgman) e mede o que sobrou.
 *
 * Substitui uma amostragem de 0,5 em 0,5 mm, que deixava passar ponta de
 * triangulo menor que o passo - foi assim que J1 entrou numa area proibida e so o
 * DRC do KiCad viu. Exata, e continua: o recozimento ainda sente o gradiente. */
static int recorta(const double *ent, int n, double *sai, int lado, double v)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        double ax = ent[i * 2], ay = ent[i * 2 + 1];
        double bx = ent[((i + 1) % n) * 2], by = ent[((i + 1) % n) * 2 + 1];
        /* lado: 0 x>=v, 1 x<=v, 2 y>=v, 3 y<=v */
        double da = (lado == 0) ? ax - v : (lado == 1) ? v - ax : (lado == 2) ? ay - v : v - ay;
        double db = (lado == 0) ? bx - v : (lado == 1) ? v - bx : (lado == 2) ? by - v : v - by;
        if (da >= 0) { sai[m * 2] = ax; sai[m * 2 + 1] = ay; m++; }
        if ((da >= 0) != (db >= 0)) {
            double t = da / (da - db);
            sai[m * 2] = ax + (bx - ax) * t;
            sai[m * 2 + 1] = ay + (by - ay) * t;
            m++;
        }
    }
    return m;
}

static double area_no_retangulo(const double *p, int n,
                                double x0, double y0, double x1, double y1)
{
    double a[256], b[256], area = 0.0;
    int m, i;
    if (n > 60) return 0.0;               /* poligono de regra real tem poucos pontos */
    memcpy(a, p, (size_t)n * 2 * sizeof(double));
    m = recorta(a, n, b, 0, x0);
    m = recorta(b, m, a, 1, x1);
    m = recorta(a, m, b, 2, y0);
    m = recorta(b, m, a, 3, y1);
    for (i = 0; i < m; i++)
        area += a[i * 2] * a[((i + 1) % m) * 2 + 1] - a[((i + 1) % m) * 2] * a[i * 2 + 1];
    return fabs(area) / 2.0;
}

/* So area que proibe FOOTPRINT - o KiCad tem essa proibicao separada da de
 * trilha. E so peca que pode sair de la. */
static double em_proibida(const Ctx *c)
{
    double total = 0.0;
    int z, i;
    for (z = 0; z < c->n_proibidas; z++) {
        const PbProibida *pz = &c->proibidas[z];
        if (!pz->sem_footprint) continue;
        for (i = 0; i < c->n_pecas; i++) {
            double x0, y0, x1, y1;
            if (!c->pecas[i].movel) continue;
            caixa(&c->pecas[i], &x0, &y0, &x1, &y1);
            total += area_no_retangulo(pz->pontos, pz->n_pontos, x0, y0, x1, y1);
        }
    }
    return total;
}

/* Cobre de peca movel perto demais da borda, somado em mm. O DRC do KiCad pegou
 * um pad de R4 colado no Edge.Cuts: a caixa do corpo estava dentro da placa, mas
 * o pad nao respeitava a folga de borda que o projeto pede. */
static double pads_na_borda(const Ctx *c)
{
    double total = 0.0;
    int i;
    for (i = 0; i < c->n_pads; i++) {
        const PbPadC *pad = &c->pads[i];
        double x, y, g, f;
        if (!c->pecas[pad->peca].movel) continue;
        pos_do_pad(c->pecas, pad, &x, &y);
        g = c->pecas[pad->peca].giro;
        f = (x - raio_na_direcao(pad, g, M_PI)) - (c->bx0 + c->r->borda);
        if (f < 0) total -= f;
        f = (c->bx1 - c->r->borda) - (x + raio_na_direcao(pad, g, 0.0));
        if (f < 0) total -= f;
        f = (y - raio_na_direcao(pad, g, -M_PI / 2)) - (c->by0 + c->r->borda);
        if (f < 0) total -= f;
        f = (c->by1 - c->r->borda) - (y + raio_na_direcao(pad, g, M_PI / 2));
        if (f < 0) total -= f;
    }
    return total;
}

static double custo(const Ctx *c)
{
    return comprimento(c) + espalhamento(c)
         + W_SOBREPOE * sobreposicao(c)
         + W_FORA * fora(c)
         + W_PROIBIDA * em_proibida(c)
         + W_PAD * pads_colados(c)
         + W_PAD * pads_na_borda(c);
}

/* Posicao que se pode montar? O juiz so roteia o que passar daqui.
 *
 * Faltava: o juiz escolhia so por "quem fecha mais ligacoes", e posicao com peca
 * empilhada pode muito bem rotear melhor - foi a que venceu e foi parar na tela.
 * Restricao fisica nao entra na balanca com comprimento de fio. */
static int legal(const Ctx *c)
{
    return sobreposicao(c) == 0.0 && fora(c) == 0.0
        && em_proibida(c) == 0.0 && pads_colados(c) < 1e-6
        && pads_na_borda(c) < 1e-6;
}

/* ------------------------------------------------------------------ recozimento */

static double arredonda(double v)
{
    return floor(v / GRADE_POS + 0.5) * GRADE_POS;
}

static void recoze(Ctx *c, int passos, unsigned long long *s)
{
    int *moveis, n_moveis = 0, i, passo;
    PbPeca *melhor_pose;
    double atual, melhor, t0, alcance_max;

    moveis = (int *)malloc((size_t)(c->n_pecas > 0 ? c->n_pecas : 1) * sizeof(int));
    for (i = 0; i < c->n_pecas; i++)
        if (c->pecas[i].movel) moveis[n_moveis++] = i;
    if (n_moveis == 0) { free(moveis); return; }

    melhor_pose = (PbPeca *)malloc((size_t)c->n_pecas * sizeof(PbPeca));
    memcpy(melhor_pose, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
    atual = melhor = custo(c);
    alcance_max = ((c->bx1 - c->bx0) > (c->by1 - c->by0)
                   ? (c->bx1 - c->bx0) : (c->by1 - c->by0)) / 4.0;
    t0 = atual / n_moveis * 0.5;
    if (t0 < 1.0) t0 = 1.0;

    for (passo = 0; passo < passos; passo++) {
        double frac = (double)passo / passos;
        double t = t0 * pow(0.02 / t0, frac);
        double alcance = alcance_max * (1.0 - 0.8 * frac);
        int a = moveis[proximo(s) % (unsigned long long)n_moveis];
        PbPeca guarda_a = c->pecas[a], guarda_b;
        int b = -1;
        double sort = sorteio(s), novo, d;

        if (sort < 0.70) {
            c->pecas[a].x = arredonda(c->pecas[a].x + (sorteio(s) * 2 - 1) * alcance);
            c->pecas[a].y = arredonda(c->pecas[a].y + (sorteio(s) * 2 - 1) * alcance);
        } else if (sort < 0.90) {
            c->pecas[a].giro = 90.0 * (double)(proximo(s) % 4);
        } else {
            double tx, ty;
            b = moveis[proximo(s) % (unsigned long long)n_moveis];
            guarda_b = c->pecas[b];
            tx = c->pecas[a].x; ty = c->pecas[a].y;
            c->pecas[a].x = c->pecas[b].x; c->pecas[a].y = c->pecas[b].y;
            c->pecas[b].x = tx; c->pecas[b].y = ty;
        }

        novo = custo(c);
        d = novo - atual;
        if (d <= 0 || sorteio(s) < exp(-d / (t > 1e-9 ? t : 1e-9))) {
            atual = novo;
            if (novo < melhor) {
                melhor = novo;
                memcpy(melhor_pose, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
            }
        } else {
            c->pecas[a] = guarda_a;
            if (b >= 0) c->pecas[b] = guarda_b;
        }
    }

    memcpy(c->pecas, melhor_pose, (size_t)c->n_pecas * sizeof(PbPeca));
    free(melhor_pose);
    free(moveis);
}

/* ------------------------------------------------------------------ juiz */

/* Monta o mapa da pose atual e roteia. Devolve quantas ligacoes ficaram abertas
 * e preenche vias/comprimento para desempate. */
static int julga(const Ctx *c, double x0, double y0, double larg, double alt,
                 const double *contorno, int n_contorno,
                 int *vias_out, double *fio_out)
{
    PbMapa *m = pb_cria(c->r, x0, y0, larg, alt);
    PbForma *formas;
    PbAlvo *alvos;
    PbSeg *segs;
    PbViaOut *vias;
    PbAberta *abertas;
    int nf = 0, na = 0, i, ns = 0, nv = 0, nab = 0;
    double fio = 0.0;

    if (!m) return 1 << 20;
    formas = (PbForma *)calloc((size_t)(c->n_pads * 2 + c->n_proibidas + 1), sizeof(PbForma));
    alvos = (PbAlvo *)calloc((size_t)(c->n_pads > 0 ? c->n_pads : 1), sizeof(PbAlvo));

    if (n_contorno >= 3) {
        formas[nf].forma = PB_FORA_DE; formas[nf].rede = PB_DE_NINGUEM;
        formas[nf].faces = PB_AMBAS;
        formas[nf].folga_extra = c->r->borda > c->r->folga ? c->r->borda - c->r->folga : 0.0;
        formas[nf].n_pontos = n_contorno; formas[nf].pontos = contorno;
        nf++;
    }
    for (i = 0; i < c->n_proibidas; i++) {
        if (!c->proibidas[i].faces_trilha) continue;   /* ha area que barra so via */
        formas[nf].forma = PB_POLIGONO; formas[nf].rede = PB_DE_NINGUEM;
        formas[nf].faces = c->proibidas[i].faces_trilha;
        formas[nf].n_pontos = c->proibidas[i].n_pontos;
        formas[nf].pontos = c->proibidas[i].pontos;
        nf++;
    }
    for (i = 0; i < c->n_pads; i++) {
        const PbPadC *pad = &c->pads[i];
        double x, y, giro = c->pecas[pad->peca].giro + pad->giro_local;
        pos_do_pad(c->pecas, pad, &x, &y);
        /* embaixo o pad e da rede dele: a trilha chega */
        formas[nf].forma = pad->forma; formas[nf].rede = pad->rede;
        formas[nf].faces = PB_BAIXO;
        formas[nf].x = x; formas[nf].y = y;
        formas[nf].larg = pad->larg; formas[nf].alt = pad->alt; formas[nf].giro = giro;
        nf++;
        /* em cima e intransponivel ATE para a propria rede: e a regra do projeto */
        formas[nf] = formas[nf - 1];
        formas[nf].rede = PB_DE_NINGUEM; formas[nf].faces = PB_CIMA;
        nf++;
        if (pad->rede) {
            alvos[na].rede = pad->rede;
            alvos[na].c = pb_coluna_de(m, x);
            alvos[na].l = pb_linha_de(m, y);
            na++;
        }
    }
    pb_pinta(m, formas, nf);

    segs = (PbSeg *)malloc(20000 * sizeof(PbSeg));
    vias = (PbViaOut *)malloc(4000 * sizeof(PbViaOut));
    abertas = (PbAberta *)malloc(2000 * sizeof(PbAberta));
    pb_roteia_tudo(m, alvos, na, segs, 20000, vias, 4000, abertas, 2000, &ns, &nv, &nab);

    for (i = 0; i < ns; i++)
        fio += hypot(pb_x_de(m, segs[i].c2) - pb_x_de(m, segs[i].c1),
                     pb_y_de(m, segs[i].l2) - pb_y_de(m, segs[i].l1));

    free(segs); free(vias); free(abertas); free(formas); free(alvos);
    pb_destroi(m);
    *vias_out = nv;
    *fio_out = fio;
    return nab;
}

/* ------------------------------------------------------------------ api */

int pb_posiciona(const PbRegras *r,
                 double x0, double y0, double larg, double alt,
                 const double *contorno, int n_contorno,
                 const PbProibida *proibidas, int n_proibidas,
                 PbPeca *pecas, int n_pecas,
                 const PbPadC *pads, int n_pads,
                 int candidatos, int passos, unsigned long long semente,
                 PbPlacar *placar)
{
    Ctx c;
    PbPeca *inicial, *vencedor;
    int k, i, melhor_ab, melhor_vias;
    double melhor_fio;
    unsigned long long s = semente ? semente : 0x9E3779B97F4A7C15ULL;

    memset(&c, 0, sizeof(c));
    c.r = r;
    c.proibidas = proibidas; c.n_proibidas = n_proibidas;
    c.pecas = pecas; c.n_pecas = n_pecas;
    c.pads = pads; c.n_pads = n_pads;

    if (n_contorno >= 3) {
        c.bx0 = c.bx1 = contorno[0];
        c.by0 = c.by1 = contorno[1];
        for (i = 1; i < n_contorno; i++) {
            double x = contorno[i * 2], y = contorno[i * 2 + 1];
            if (x < c.bx0) c.bx0 = x;
            if (x > c.bx1) c.bx1 = x;
            if (y < c.by0) c.by0 = y;
            if (y > c.by1) c.by1 = y;
        }
    } else {
        c.bx0 = x0; c.by0 = y0; c.bx1 = x0 + larg; c.by1 = y0 + alt;
    }

    c.redes = (int *)malloc((size_t)(n_pads > 0 ? n_pads : 1) * sizeof(int));
    for (i = 0; i < n_pads; i++) {
        int j, ja = 0;
        if (!pads[i].rede) continue;
        for (j = 0; j < c.n_redes; j++) if (c.redes[j] == pads[i].rede) { ja = 1; break; }
        if (!ja) c.redes[c.n_redes++] = pads[i].rede;
    }

    inicial = (PbPeca *)malloc((size_t)n_pecas * sizeof(PbPeca));
    vencedor = (PbPeca *)malloc((size_t)n_pecas * sizeof(PbPeca));
    memcpy(inicial, pecas, (size_t)n_pecas * sizeof(PbPeca));

    placar->descartados = 0;
    /* candidato 0: a posicao de entrada, intocada */
    melhor_ab = julga(&c, x0, y0, larg, alt, contorno, n_contorno, &melhor_vias, &melhor_fio);
    memcpy(vencedor, pecas, (size_t)n_pecas * sizeof(PbPeca));
    placar->abertas_antes = melhor_ab;
    placar->vencedor = 0;

    for (k = 1; k <= candidatos; k++) {
        int ab, nv;
        double fio;
        memcpy(pecas, inicial, (size_t)n_pecas * sizeof(PbPeca));
        s ^= (unsigned long long)k * 0x9E3779B97F4A7C15ULL;
        recoze(&c, passos, &s);
        if (!legal(&c)) {
            placar->descartados++;
            continue;
        }
        ab = julga(&c, x0, y0, larg, alt, contorno, n_contorno, &nv, &fio);
        /* quem fecha mais ganha; empate, menos via; empate, menos fio */
        if (ab < melhor_ab
                || (ab == melhor_ab && nv < melhor_vias)
                || (ab == melhor_ab && nv == melhor_vias && fio < melhor_fio - 1e-6)) {
            melhor_ab = ab; melhor_vias = nv; melhor_fio = fio;
            memcpy(vencedor, pecas, (size_t)n_pecas * sizeof(PbPeca));
            placar->vencedor = k;
        }
    }

    memcpy(pecas, vencedor, (size_t)n_pecas * sizeof(PbPeca));
    placar->abertas_depois = melhor_ab;
    placar->vias = melhor_vias;
    placar->fio = melhor_fio;

    free(inicial); free(vencedor); free(c.redes);
    return melhor_ab;
}
