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
 *
 * E como na perfboard: sorteia sem parar, em todos os nucleos, ate um candidato
 * fechar a placa inteira ou o prazo acabar. Cada rede que nao fecha num sorteio
 * passa a pesar mais no custo dos proximos, que entao aproximam os pads dela.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif
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
/* Por mm de peca na frente do conector (ver conectores_por_dentro). Forte o
 * bastante para vencer o fio que o conector puxa: na blink blink, sem isto, J1
 * foi parar no meio da placa - fechou, mas e incomodo e feio. */
#define W_CONECTOR  40.0
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

static void pos_do_pad(const PbPeca *pecas, const PbPadC *pad, double *x, double *y)
{
    const PbPeca *p = &pecas[pad->peca];
    double a = -p->giro * M_PI / 180.0, ca = cos(a), sa = sin(a);   /* local -> mundo */
    double dy = p->lado ? -pad->dy : pad->dy;      /* virada: espelho como o KiCad */
    *x = p->x + pad->dx * ca - dy * sa;
    *y = p->y + pad->dx * sa + dy * ca;
}

/* Giro do pad no mundo: na peca virada, o giro proprio do pad troca de sinal. */
static double giro_do_pad(const PbPeca *pecas, const PbPadC *pad)
{
    const PbPeca *p = &pecas[pad->peca];
    return p->giro + (p->lado ? -pad->giro_local : pad->giro_local);
}

/* ------------------------------------------------------------------ custo */

typedef struct {
    const PbRegras *r;
    double bx0, by0, bx1, by1;          /* caixa do contorno */
    const PbProibida *proibidas;
    int n_proibidas;
    const PbPlano *planos;
    int n_planos;
    volatile int *parar;                 /* o pedido de parar do placar */
    PbPeca *pecas;
    int n_pecas;
    const PbPadC *pads;
    int n_pads;
    int *redes;                          /* redes distintas */
    int n_redes;
    const double *peso;                  /* por rede (indice em `redes`); NULL = 1 */

    /* Corpo de cada peca: casco convexo do courtyard, no referencial da peca. */
    double *casco;                       /* MAX_CASCO pontos por peca */
    const int *n_casco;
    /* O mesmo casco ja no mundo, refeito a cada avaliacao. Rascunho de cada
     * thread - por isso ponteiro, e nao parte do que e compartilhado. */
    double *mundo;                       /* MAX_CASCO pontos por peca */
    double *caixas;                      /* x0 y0 x1 y1 por peca */
} Ctx;

/* ------------------------------------------------------------------ corpo */

/* O corpo e o COURTYARD de verdade, nao a caixa dele.
 *
 * A caixa errava duas vezes, e as duas reprovavam a placa feita a mao, que o DRC
 * do KiCad aprova: o BBox() do KiCad inclui a largura da linha do courtyard (0,05
 * mm de cada lado), e R3 e R4, que tem 0,01 mm de vao, saiam sobrepostos; e a
 * caixa de um courtyard REDONDO (eletrolitico, furo de fixacao) tem cantos que o
 * circulo nao tem, e os cantos caiam nas areas proibidas triangulares. Com a
 * posicao de entrada ilegal, 95% dos sorteios iam para o lixo sem ser roteados.
 *
 * Casco convexo e nao o poligono cru: courtyard concavo vira o casco dele - mais
 * severo, nunca mais frouxo -, e casco convexo e o que o recorte exato aceita. */
#define MAX_CASCO 64

static double area_com_sinal(const double *p, int n)
{
    double a = 0.0;
    int i;
    for (i = 0; i < n; i++)
        a += p[i * 2] * p[((i + 1) % n) * 2 + 1] - p[((i + 1) % n) * 2] * p[i * 2 + 1];
    return a / 2.0;
}

static int compara_pontos(const void *a, const void *b)
{
    const double *p = (const double *)a, *q = (const double *)b;
    if (p[0] != q[0]) return p[0] < q[0] ? -1 : 1;
    if (p[1] != q[1]) return p[1] < q[1] ? -1 : 1;
    return 0;
}

static double vira(const double *o, const double *a, const double *b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

/* Cadeia monotona. `p` e reordenado. Devolve quantos pontos foram para `sai`
 * (que precisa de espaco para 2n pontos). */
static int casco_convexo(double *p, int n, double *sai)
{
    int i, k = 0, t;
    if (n < 3) { memcpy(sai, p, (size_t)n * 2 * sizeof(double)); return n; }
    qsort(p, (size_t)n, 2 * sizeof(double), compara_pontos);
    for (i = 0; i < n; i++) {
        while (k >= 2 && vira(&sai[(k - 2) * 2], &sai[(k - 1) * 2], &p[i * 2]) <= 0) k--;
        sai[k * 2] = p[i * 2]; sai[k * 2 + 1] = p[i * 2 + 1]; k++;
    }
    for (i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && vira(&sai[(k - 2) * 2], &sai[(k - 1) * 2], &p[i * 2]) <= 0) k--;
        sai[k * 2] = p[i * 2]; sai[k * 2 + 1] = p[i * 2 + 1]; k++;
    }
    return k - 1;       /* o ultimo repete o primeiro */
}

/* Vao minimo entre corpos, metade em cada um. A sobreposicao e medida por AREA,
 * e courtyard so encostado tem area zero - mas o KiCad acusa encostado como
 * courtyards_overlap: foi o que pegou D1 e J1 lado a lado na mesma coordenada.
 * Pequeno de proposito: a placa feita a mao tem R3 e R4 a 0,01 mm, e o DRC
 * aceita. */
#define VAO_CORPO 0.005

/* Afasta cada aresta do poligono convexo `d` mm para fora (canto em esquadria):
 * para convexo, e o deslocamento exato do contorno. */
static void infla(double *p, int n, double d)
{
    double t[MAX_CASCO * 2], s = area_com_sinal(p, n) >= 0 ? 1.0 : -1.0;
    int i;
    if (n < 3) return;
    for (i = 0; i < n; i++) {
        const double *a = &p[((i + n - 1) % n) * 2], *b = &p[i * 2], *c = &p[((i + 1) % n) * 2];
        double e1x = b[0] - a[0], e1y = b[1] - a[1], e2x = c[0] - b[0], e2y = c[1] - b[1];
        double l1 = hypot(e1x, e1y), l2 = hypot(e2x, e2y), n1x, n1y, n2x, n2y, k;
        if (l1 < 1e-12 || l2 < 1e-12) { t[i * 2] = b[0]; t[i * 2 + 1] = b[1]; continue; }
        /* normal para fora: (dy, -dx) no sentido anti-horario do Y para cima */
        n1x = s * e1y / l1; n1y = -s * e1x / l1;
        n2x = s * e2y / l2; n2y = -s * e2x / l2;
        k = 1.0 + n1x * n2x + n1y * n2y;
        if (k < 0.1) k = 0.1;
        t[i * 2] = b[0] + d * (n1x + n2x) / k;
        t[i * 2 + 1] = b[1] + d * (n1y + n2y) / k;
    }
    memcpy(p, t, (size_t)n * 2 * sizeof(double));
}

/* Leva todos os cascos para o mundo e anota a caixa de cada um. Uma vez por
 * avaliacao de custo: sobreposicao, borda e area proibida leem daqui. */
static void corpo_no_mundo(const Ctx *c, int i);

static void corpos_no_mundo(const Ctx *c)
{
    int i;
    for (i = 0; i < c->n_pecas; i++) corpo_no_mundo(c, i);
}

static void corpo_no_mundo(const Ctx *c, int i)
{
    int k;
    {
        const PbPeca *p = &c->pecas[i];
        const double *l = &c->casco[i * MAX_CASCO * 2];
        double *w = &c->mundo[i * MAX_CASCO * 2], *bx = &c->caixas[i * 4];
        double a = -p->giro * M_PI / 180.0, ca = cos(a), sa = sin(a);   /* local -> mundo */
        double esp = p->lado ? -1.0 : 1.0;          /* virada: espelho do y local */
        bx[0] = bx[1] = 1e300; bx[2] = bx[3] = -1e300;
        for (k = 0; k < c->n_casco[i]; k++) {
            w[k * 2] = p->x + l[k * 2] * ca - esp * l[k * 2 + 1] * sa;
            w[k * 2 + 1] = p->y + l[k * 2] * sa + esp * l[k * 2 + 1] * ca;
        }
        infla(w, c->n_casco[i], VAO_CORPO / 2.0);
        for (k = 0; k < c->n_casco[i]; k++) {
            double x = w[k * 2], y = w[k * 2 + 1];
            if (x < bx[0]) bx[0] = x;
            if (y < bx[1]) bx[1] = y;
            if (x > bx[2]) bx[2] = x;
            if (y > bx[3]) bx[3] = y;
        }
    }
}

static void caixa(const Ctx *c, int i, double *x0, double *y0, double *x1, double *y1)
{
    const double *bx = &c->caixas[i * 4];
    *x0 = bx[0]; *y0 = bx[1]; *x1 = bx[2]; *y1 = bx[3];
}

/* Area EXATA do poligono `sujeito` dentro do poligono CONVEXO `convexo`:
 * Sutherland-Hodgman, um semiplano por aresta do convexo, e mede o que sobrou.
 * O sujeito pode ser qualquer (area proibida concava inclusive). */
#define MAX_RECORTE 512

static int semiplano(const double *ent, int n, double *sai,
                     const double *a, const double *b, double sinal)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const double *p = &ent[i * 2], *q = &ent[((i + 1) % n) * 2];
        double dp = sinal * vira(a, b, p), dq = sinal * vira(a, b, q);
        if (dp >= 0 && m < MAX_RECORTE) { sai[m * 2] = p[0]; sai[m * 2 + 1] = p[1]; m++; }
        if ((dp >= 0) != (dq >= 0) && m < MAX_RECORTE) {
            double t = dp / (dp - dq);
            sai[m * 2] = p[0] + (q[0] - p[0]) * t;
            sai[m * 2 + 1] = p[1] + (q[1] - p[1]) * t;
            m++;
        }
    }
    return m;
}

static double area_comum(const double *sujeito, int ns, const double *convexo, int nc)
{
    double a[MAX_RECORTE * 2], b[MAX_RECORTE * 2], sinal;
    int m = ns, k;
    if (ns < 3 || nc < 3 || ns > MAX_RECORTE) return 0.0;
    sinal = area_com_sinal(convexo, nc) >= 0 ? 1.0 : -1.0;
    memcpy(a, sujeito, (size_t)ns * 2 * sizeof(double));
    for (k = 0; k < nc && m > 0; k++) {
        m = semiplano(a, m, b, &convexo[k * 2], &convexo[((k + 1) % nc) * 2], sinal);
        memcpy(a, b, (size_t)m * 2 * sizeof(double));
    }
    return m >= 3 ? fabs(area_com_sinal(a, m)) : 0.0;
}

/* Quanto o pad avanca na direcao `ang` (radianos, mundo). A pergunta nunca e "qual
 * o tamanho do pad", e sim quanto ele avanca NA DIRECAO do vizinho: num oval de
 * 1,8 x 3 virado para o pino ao lado, usar o eixo maior erra o vao em mais de 1 mm. */
static double raio_na_direcao(const PbPadC *pad, double giro_pad, double ang)
{
    double a = pad->larg / 2.0, b = pad->alt / 2.0;
    double t = ang + giro_pad * M_PI / 180.0;                      /* mundo -> local */
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
            falta = raio_na_direcao(pa, giro_do_pad(c->pecas, pa), ang)
                  + raio_na_direcao(pb, giro_do_pad(c->pecas, pb), ang + M_PI)
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
        if (n >= 2)
            total += (c->peso ? c->peso[k] : 1.0) * ((mxx - mnx) + (mxy - mny));
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
        caixa(c, i, &x0, &y0, &x1, &y1);
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
/* Quanto os pads da peca `k` entram no corpo da peca `i`, somado em mm (a
 * penetracao de cada pad, contando o raio dele). */
static double pads_sob_corpo(const Ctx *c, int i, int k)
{
    const double *h = &c->mundo[i * MAX_CASCO * 2];
    int n = c->n_casco[i], j, e;
    double sinal = area_com_sinal(h, n) >= 0 ? 1.0 : -1.0, total = 0.0;
    for (j = 0; j < c->n_pads; j++) {
        const PbPadC *pad = &c->pads[j];
        double x, y, r, menor = 1e300;
        if (pad->peca != k) continue;
        pos_do_pad(c->pecas, pad, &x, &y);
        r = (pad->larg > pad->alt ? pad->larg : pad->alt) / 2.0;
        for (e = 0; e < n; e++) {
            const double *a = &h[e * 2], *b = &h[((e + 1) % n) * 2];
            double ex = b[0] - a[0], ey = b[1] - a[1], len = sqrt(ex * ex + ey * ey);
            double dist;
            if (len < 1e-12) continue;
            dist = sinal * (ex * (y - a[1]) - ey * (x - a[0])) / len;   /* + = dentro */
            if (dist < menor) menor = dist;
        }
        if (menor > -r) total += menor + r;
    }
    return total;
}

static double sobreposicao(const Ctx *c)
{
    double total = 0.0;
    int i, k;
    for (i = 0; i < c->n_pecas; i++) {
        double a0, b0, a1, b1;
        caixa(c, i, &a0, &b0, &a1, &b1);
        for (k = i + 1; k < c->n_pecas; k++) {
            double c0, d0, c1, d1, area;
            if (!c->pecas[i].movel && !c->pecas[k].movel) continue;
            caixa(c, k, &c0, &d0, &c1, &d1);
            if (a1 <= c0 || c1 <= a0 || b1 <= d0 || d1 <= b0) continue;   /* longe */
            if (c->pecas[i].lado != c->pecas[k].lado) {
                /* Lados opostos: os corpos podem dividir a mesma area, mas o
                 * terminal de uma peca THT atravessa e e soldado do outro lado -
                 * nenhum corpo pode cobrir pad (solda) da outra. */
                total += pads_sob_corpo(c, i, k) + pads_sob_corpo(c, k, i);
                continue;
            }
            area = area_comum(&c->mundo[i * MAX_CASCO * 2], c->n_casco[i],
                              &c->mundo[k * MAX_CASCO * 2], c->n_casco[k]);
            if (area > TOLERA_SOBREPOSICAO) total += area;
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
        caixa(c, i, &x0, &y0, &x1, &y1);
        if (c->bx0 > x0) total += c->bx0 - x0;
        if (x1 > c->bx1) total += x1 - c->bx1;
        if (c->by0 > y0) total += c->by0 - y0;
        if (y1 > c->by1) total += y1 - c->by1;
    }
    return total;
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
            if (!c->pecas[i].movel) continue;
            total += area_comum(pz->pontos, pz->n_pontos,
                                &c->mundo[i * MAX_CASCO * 2], c->n_casco[i]);
        }
    }
    return total;
}

/* Cobre de peca movel perto demais da borda, somado em mm. O DRC do KiCad pegou
 * um pad de R4 colado no Edge.Cuts: a caixa do corpo estava dentro da placa, mas
 * o pad nao respeitava a folga de borda que o projeto pede. */
/* Conector na PERIFERIA do circuito: em algum dos quatro lados, nenhuma outra
 * peca fica entre ele e a borda. Nao e encostar na borda - isso atrapalha a
 * fresa -, e ser a peca mais de fora.
 *
 * Para cada lado: das pecas que dividem a faixa do conector (se sobrepoem a ele
 * no outro eixo), quanto a que mais avanca alem dele naquele sentido passa da
 * face dele. Zero = ninguem na frente. Fica o lado mais favoravel, e soma-se em
 * mm sobre os conectores. Furo de fixacao nao conta: e mecanico, fica no canto. */
static double conectores_por_dentro(const Ctx *c)
{
    double total = 0.0;
    int i, k, lado;
    for (i = 0; i < c->n_pecas; i++) {
        double x0, y0, x1, y1, melhor = 1e300;
        if (c->pecas[i].papel != PB_PAPEL_CONECTOR || !c->pecas[i].movel) continue;
        caixa(c, i, &x0, &y0, &x1, &y1);
        for (lado = 0; lado < 4; lado++) {
            double pior = 0.0;
            for (k = 0; k < c->n_pecas; k++) {
                double a0, b0, a1, b1, passa;
                if (k == i || c->pecas[k].papel == PB_PAPEL_MECANICA) continue;
                caixa(c, k, &a0, &b0, &a1, &b1);
                if (lado < 2 && (b1 <= y0 || b0 >= y1)) continue;   /* fora da faixa */
                if (lado >= 2 && (a1 <= x0 || a0 >= x1)) continue;
                passa = lado == 0 ? x0 - a0      /* esquerda */
                      : lado == 1 ? a1 - x1      /* direita */
                      : lado == 2 ? y0 - b0      /* cima */
                      :             b1 - y1;     /* baixo */
                if (passa > pior) pior = passa;
            }
            if (pior < melhor) melhor = pior;
        }
        total += melhor;
    }
    return total;
}

static double pads_na_borda(const Ctx *c)
{
    double total = 0.0;
    int i;
    for (i = 0; i < c->n_pads; i++) {
        const PbPadC *pad = &c->pads[i];
        double x, y, g, f;
        if (!c->pecas[pad->peca].movel) continue;
        pos_do_pad(c->pecas, pad, &x, &y);
        g = giro_do_pad(c->pecas, pad);
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

/* Custo geometrico. Se `eh_legal` vier, diz tambem se a posicao se monta - com
 * as mesmas contas, de graca, em vez de refazer tudo em legal(). */
static double custo_e_lei(const Ctx *c, int *eh_legal)
{
    double sobre, fo, pr, cola, bo;
    corpos_no_mundo(c);           /* o resto le os corpos ja no mundo */
    sobre = sobreposicao(c); fo = fora(c); pr = em_proibida(c);
    cola = pads_colados(c); bo = pads_na_borda(c);
    if (eh_legal)
        *eh_legal = sobre == 0.0 && fo == 0.0 && pr == 0.0 && cola < 1e-6 && bo < 1e-6;
    return comprimento(c) + espalhamento(c)
         + W_SOBREPOE * sobre + W_FORA * fo + W_PROIBIDA * pr
         + W_PAD * cola + W_PAD * bo
         + W_CONECTOR * conectores_por_dentro(c);
}


/* Posicao que se pode montar? O juiz so roteia o que passar daqui.
 *
 * Faltava: o juiz escolhia so por "quem fecha mais ligacoes", e posicao com peca
 * empilhada pode muito bem rotear melhor - foi a que venceu e foi parar na tela.
 * Restricao fisica nao entra na balanca com comprimento de fio. */
static int legal(const Ctx *c)
{
    int ok;
    custo_e_lei(c, &ok);
    return ok;
}

/* ------------------------------------------------------------------ recozimento */

/* Giro sorteado: 3 em cada 4, multiplo de 90; 1 em 4, na diagonal (45, 135...).
 * Peca a 45 graus ocupa mais caixa em volta - tem de aparecer quando ajuda, nao
 * por padrao. O resto do nucleo (corpo, pads, chegada no pad, raio termico) ja
 * trata qualquer angulo. */
static double giro_sorteado(unsigned long long *s)
{
    if (sorteio(s) < 0.75) return 90.0 * (double)(proximo(s) % 4);
    return 45.0 + 90.0 * (double)(proximo(s) % 4);
}

static double arredonda(double v)
{
    return floor(v / GRADE_POS + 0.5) * GRADE_POS;
}

/* Recozimento no custo geometrico.
 *
 * Devolve a melhor posicao LEGAL que visitou, e so cai para a melhor de todas se
 * nenhuma legal apareceu. Antes devolvia a mais barata, e a mais barata quase
 * sempre tinha um pad encostado em outro por decimos de mm: medido na blink
 * blink, 6140 de 6280 sorteios iam para o lixo sem nem serem roteados. */
static void recoze(Ctx *c, int passos, unsigned long long *s, double fator_t0,
                   const unsigned char *congeladas)
{
    int *moveis, n_moveis = 0, i, passo, achou_legal = 0, ok;
    PbPeca *melhor_pose, *melhor_legal;
    double atual, melhor, melhor_lei = 1e300, t0, alcance_max;

    moveis = (int *)malloc((size_t)(c->n_pecas > 0 ? c->n_pecas : 1) * sizeof(int));
    for (i = 0; i < c->n_pecas; i++)
        if (c->pecas[i].movel && !(congeladas && congeladas[i])) moveis[n_moveis++] = i;
    if (n_moveis == 0) { free(moveis); return; }

    melhor_pose = (PbPeca *)malloc((size_t)c->n_pecas * sizeof(PbPeca));
    melhor_legal = (PbPeca *)malloc((size_t)c->n_pecas * sizeof(PbPeca));
    memcpy(melhor_pose, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
    atual = melhor = custo_e_lei(c, &ok);
    if (ok) {
        achou_legal = 1; melhor_lei = atual;
        memcpy(melhor_legal, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
    }
    alcance_max = ((c->bx1 - c->bx0) > (c->by1 - c->by0)
                   ? (c->bx1 - c->bx0) : (c->by1 - c->by0)) / 4.0;
    t0 = atual / n_moveis * 0.5;
    if (t0 < 1.0) t0 = 1.0;
    t0 *= fator_t0;                 /* reparo: morno, para nao desmontar a melhor */
    if (t0 < 0.05) t0 = 0.05;

    for (passo = 0; passo < passos; passo++) {
        double frac = (double)passo / passos;
        double t = t0 * pow(0.02 / t0, frac);
        double alcance = alcance_max * (1.0 - 0.8 * frac);
        int a = moveis[proximo(s) % (unsigned long long)n_moveis];
        PbPeca guarda_a = c->pecas[a], guarda_b;
        int b = -1;
        double sort = sorteio(s), novo, d;

        if (sort < 0.64) {
            c->pecas[a].x = arredonda(c->pecas[a].x + (sorteio(s) * 2 - 1) * alcance);
            c->pecas[a].y = arredonda(c->pecas[a].y + (sorteio(s) * 2 - 1) * alcance);
        } else if (sort < 0.90) {
            /* gira; e, se o usuario deixou, as vezes vira a peca de lado */
            if (c->pecas[a].pode_virar && sorteio(s) < 0.35)
                c->pecas[a].lado = !c->pecas[a].lado;
            else
                c->pecas[a].giro = giro_sorteado(s);
        } else {
            double tx, ty;
            b = moveis[proximo(s) % (unsigned long long)n_moveis];
            guarda_b = c->pecas[b];
            tx = c->pecas[a].x; ty = c->pecas[a].y;
            c->pecas[a].x = c->pecas[b].x; c->pecas[a].y = c->pecas[b].y;
            c->pecas[b].x = tx; c->pecas[b].y = ty;
        }

        novo = custo_e_lei(c, &ok);
        d = novo - atual;
        if (d <= 0 || sorteio(s) < exp(-d / (t > 1e-9 ? t : 1e-9))) {
            atual = novo;
            if (novo < melhor) {
                melhor = novo;
                memcpy(melhor_pose, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
            }
            if (ok && novo < melhor_lei) {
                achou_legal = 1; melhor_lei = novo;
                memcpy(melhor_legal, c->pecas, (size_t)c->n_pecas * sizeof(PbPeca));
            }
        } else {
            c->pecas[a] = guarda_a;
            if (b >= 0) c->pecas[b] = guarda_b;
        }
    }

    memcpy(c->pecas, achou_legal ? melhor_legal : melhor_pose,
           (size_t)c->n_pecas * sizeof(PbPeca));
    free(melhor_pose);
    free(melhor_legal);
    free(moveis);
}

/* ------------------------------------------------------------------ juiz */

/* Monta o mapa da pose atual e roteia. Devolve quantas ligacoes ficaram abertas
 * e preenche vias/comprimento para desempate. */
static int julga(const Ctx *c, double x0, double y0, double larg, double alt,
                 const double *contorno, int n_contorno,
                 int *vias_out, double *fio_out, int *falhas, double *perda_out,
                 PbMapa **reuso)
{
    PbMapa *m;
    PbForma *formas;
    PbAlvo *alvos;
    PbSeg *segs;
    PbViaOut *vias;
    PbAberta *abertas;
    int nf = 0, na = 0, i, ns = 0, nv = 0, nab = 0;
    double fio = 0.0;

    /* o mapa do nucleo e reaproveitado de uma posicao para a outra */
    if (reuso && *reuso) {
        m = *reuso;
        pb_reinicia(m);
    } else {
        m = pb_cria(c->r, x0, y0, larg, alt);
        if (reuso) *reuso = m;
    }
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
        double x, y, giro = giro_do_pad(c->pecas, pad);
        int virada = c->pecas[pad->peca].lado;
        pos_do_pad(c->pecas, pad, &x, &y);
        /* na face da SOLDA o pad e da rede dele: a trilha chega */
        formas[nf].forma = pad->forma; formas[nf].rede = pad->rede;
        formas[nf].faces = virada ? PB_CIMA : PB_BAIXO;
        formas[nf].x = x; formas[nf].y = y;
        formas[nf].larg = pad->larg; formas[nf].alt = pad->alt; formas[nf].giro = giro;
        nf++;
        /* do lado do corpo e intransponivel ATE para a propria rede: solda de um
         * lado so, que e a regra do projeto */
        formas[nf] = formas[nf - 1];
        formas[nf].rede = PB_DE_NINGUEM; formas[nf].faces = virada ? PB_BAIXO : PB_CIMA;
        nf++;
        if (pad->rede) {
            alvos[na].rede = pad->rede;
            alvos[na].c = pb_coluna_de(m, x);
            alvos[na].l = pb_linha_de(m, y);
            alvos[na].forma = pad->forma;
            alvos[na].x = x; alvos[na].y = y;
            alvos[na].larg = pad->larg; alvos[na].alt = pad->alt;
            alvos[na].giro = giro;
            alvos[na].raio_termico = giro + pad->raio_termico;
            alvos[na].face = virada ? 0 : 1;
            na++;
        }
    }
    pb_pinta(m, formas, nf);
    pb_define_planos(m, c->planos, c->n_planos);
    m->parar = c->parar;

    segs = (PbSeg *)malloc(20000 * sizeof(PbSeg));
    vias = (PbViaOut *)malloc(4000 * sizeof(PbViaOut));
    abertas = (PbAberta *)malloc(2000 * sizeof(PbAberta));
    pb_roteia_tudo(m, alvos, na, segs, 20000, vias, 4000, abertas, 2000, &ns, &nv, &nab);

    /* quem nao fechou, por rede: e o que o sorteio aprende */
    for (i = 0; i < nab; i++) {
        int k;
        for (k = 0; k < c->n_redes; k++)
            if (c->redes[k] == abertas[i].rede) { falhas[k]++; break; }
    }

    for (i = 0; i < ns; i++)
        fio += hypot(pb_x_de(m, segs[i].c2) - pb_x_de(m, segs[i].c1),
                     pb_y_de(m, segs[i].l2) - pb_y_de(m, segs[i].l1));

    free(segs); free(vias); free(abertas); free(formas); free(alvos);
    *perda_out = pb_plano_perdido(m);    /* antes de destruir o mapa */
    if (!reuso) pb_destroi(m);
    *vias_out = nv;
    *fio_out = fio;
    return nab;
}

/* ------------------------------------------------------------------ reparo */

/* Culpadas: pecas moveis com pad numa rede que nao fechou. */
static void marca_culpadas(const Ctx *c, const int *falhas, unsigned char *culpadas,
                           int *n_culpadas)
{
    int i, k;
    memset(culpadas, 0, (size_t)c->n_pecas);
    *n_culpadas = 0;
    for (i = 0; i < c->n_pads; i++) {
        const PbPadC *pad = &c->pads[i];
        if (!pad->rede || !c->pecas[pad->peca].movel || culpadas[pad->peca]) continue;
        for (k = 0; k < c->n_redes; k++)
            if (c->redes[k] == pad->rede && falhas[k]) {
                culpadas[pad->peca] = 1;
                (*n_culpadas)++;
                break;
            }
    }
}

/* Leva uma peca culpada (as vezes duas) para um lugar, giro e lado sorteados na
 * placa. O sorteio comum so aproxima e afasta aos poucos a partir da posicao de
 * entrada, e o custo dele (comprimento de fio) nao ve cruzamento: o J1 do
 * interfone nunca saia do canal apertado ao lado do Nano. Aqui ele salta. */
static void perturba(Ctx *c, const unsigned char *culpadas, unsigned long long *s,
                     unsigned char *movidas)
{
    int lista[256], n = 0, i, vezes;
    memset(movidas, 0, (size_t)c->n_pecas);
    for (i = 0; i < c->n_pecas && n < 256; i++)
        if (culpadas[i] && c->pecas[i].movel) lista[n++] = i;
    if (n == 0) return;
    vezes = (n > 1 && sorteio(s) < 0.3) ? 2 : 1;
    while (vezes--) {
        int qual = lista[proximo(s) % (unsigned long long)n];
        PbPeca *p = &c->pecas[qual];
        movidas[qual] = 1;
        p->x = arredonda(c->bx0 + sorteio(s) * (c->bx1 - c->bx0));
        p->y = arredonda(c->by0 + sorteio(s) * (c->by1 - c->by0));
        p->giro = giro_sorteado(s);
        if (p->pode_virar && sorteio(s) < 0.5) p->lado = !p->lado;
    }
}

/* ------------------------------------------------------------------ do zero */

/* A peca `i`, na pose que esta, cabe junto das ja colocadas? As mesmas regras do
 * legal(), olhando so ela: dentro da placa, fora de area que proibe footprint,
 * corpo sem sobrepor corpo do mesmo lado nem cobrir pad do outro lado, pads com
 * folga dos pads alheios e da borda. */
static int cabe_aqui(const Ctx *c, int i, const unsigned char *colocada)
{
    double x0, y0, x1, y1;
    int k, j, q, z;
    corpo_no_mundo(c, i);
    caixa(c, i, &x0, &y0, &x1, &y1);
    if (x0 < c->bx0 || x1 > c->bx1 || y0 < c->by0 || y1 > c->by1) return 0;
    for (z = 0; z < c->n_proibidas; z++)
        if (c->proibidas[z].sem_footprint
                && area_comum(c->proibidas[z].pontos, c->proibidas[z].n_pontos,
                              &c->mundo[i * MAX_CASCO * 2], c->n_casco[i]) > 0.0)
            return 0;
    for (k = 0; k < c->n_pecas; k++) {
        double a0, b0, a1, b1;
        if (k == i || !colocada[k]) continue;
        caixa(c, k, &a0, &b0, &a1, &b1);
        if (x1 + 3 < a0 || a1 + 3 < x0 || y1 + 3 < b0 || b1 + 3 < y0) continue;   /* longe */
        if (c->pecas[i].lado == c->pecas[k].lado) {
            if (area_comum(&c->mundo[i * MAX_CASCO * 2], c->n_casco[i],
                           &c->mundo[k * MAX_CASCO * 2], c->n_casco[k]) > TOLERA_SOBREPOSICAO)
                return 0;
        } else if (pads_sob_corpo(c, i, k) + pads_sob_corpo(c, k, i) > 0.0) {
            return 0;
        }
    }
    for (j = 0; j < c->n_pads; j++) {
        const PbPadC *pa = &c->pads[j];
        double xa, ya, ga;
        if (pa->peca != i) continue;
        pos_do_pad(c->pecas, pa, &xa, &ya);
        ga = giro_do_pad(c->pecas, pa);
        if (xa - raio_na_direcao(pa, ga, M_PI) < c->bx0 + c->r->borda
                || xa + raio_na_direcao(pa, ga, 0.0) > c->bx1 - c->r->borda
                || ya - raio_na_direcao(pa, ga, -M_PI / 2) < c->by0 + c->r->borda
                || ya + raio_na_direcao(pa, ga, M_PI / 2) > c->by1 - c->r->borda)
            return 0;
        for (q = 0; q < c->n_pads; q++) {
            const PbPadC *pb = &c->pads[q];
            double xb, yb, d, ang;
            if (pb->peca == i || !colocada[pb->peca]) continue;
            pos_do_pad(c->pecas, pb, &xb, &yb);
            d = hypot(xb - xa, yb - ya);
            if (d > 8.0) continue;
            ang = atan2(yb - ya, xb - xa);
            if (raio_na_direcao(pa, ga, ang)
                    + raio_na_direcao(pb, giro_do_pad(c->pecas, pb), ang + M_PI)
                    + c->r->folga > d)
                return 0;
        }
    }
    return 1;
}

/* Quanto fio a peca `i` puxa ate o que ja esta colocado: para cada pad dela, a
 * distancia ate o pad mais perto da mesma rede numa peca ja colocada. */
static double fio_ate_colocadas(const Ctx *c, int i, const unsigned char *colocada)
{
    double total = 0.0;
    int j, q;
    for (j = 0; j < c->n_pads; j++) {
        const PbPadC *pa = &c->pads[j];
        double xa, ya, melhor = -1.0;
        if (pa->peca != i || !pa->rede) continue;
        pos_do_pad(c->pecas, pa, &xa, &ya);
        for (q = 0; q < c->n_pads; q++) {
            const PbPadC *pb = &c->pads[q];
            double xb, yb, d;
            if (pb->peca == i || pb->rede != pa->rede || !colocada[pb->peca]) continue;
            pos_do_pad(c->pecas, pb, &xb, &yb);
            d = fabs(xb - xa) + fabs(yb - ya);
            if (melhor < 0 || d < melhor) melhor = d;
        }
        if (melhor > 0) total += melhor;
    }
    return total;
}

/* Monta a placa DO ZERO: as pecas moveis entram uma a uma, das maiores para as
 * menores (com um pouco de sorteio na ordem), cada uma no melhor lugar livre que
 * aparecer entre tentativas sorteadas de posicao, giro e lado - melhor = menos
 * fio ate o que ja entrou. As travadas ficam onde estao.
 *
 * Existe porque o recozimento sempre partia da posicao de entrada e andava aos
 * poucos: com a placa encolhida (interfone de 80x60 para 60x45) a entrada vira
 * um bolo de pecas sobrepostas e fora do contorno, e em 16 mil sorteios nenhum
 * saiu legal. Aqui a posicao antiga nao conta. */
#define ZERO_TENTATIVAS 400

static void monta_do_zero(Ctx *c, unsigned long long *s)
{
    int n = c->n_pecas, i, k, t;
    unsigned char *colocada = (unsigned char *)calloc((size_t)(n > 0 ? n : 1), 1);
    int *ordem = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    double *peso = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    int n_ordem = 0;

    corpos_no_mundo(c);
    for (i = 0; i < n; i++) {
        if (!c->pecas[i].movel) { colocada[i] = 1; continue; }
        {
            double x0, y0, x1, y1;
            caixa(c, i, &x0, &y0, &x1, &y1);
            peso[i] = (x1 - x0) * (y1 - y0) * (0.75 + 0.5 * sorteio(s));
        }
        ordem[n_ordem++] = i;
    }
    for (i = 1; i < n_ordem; i++) {            /* maiores primeiro */
        int x = ordem[i];
        for (k = i; k > 0 && peso[ordem[k - 1]] < peso[x]; k--) ordem[k] = ordem[k - 1];
        ordem[k] = x;
    }
    for (k = 0; k < n_ordem; k++) {
        PbPeca *p = &c->pecas[ordem[k]], melhor_pose = *p, ultima = *p;
        double melhor = -1.0;
        for (t = 0; t < ZERO_TENTATIVAS; t++) {
            double fio;
            p->x = arredonda(c->bx0 + sorteio(s) * (c->bx1 - c->bx0));
            p->y = arredonda(c->by0 + sorteio(s) * (c->by1 - c->by0));
            p->giro = giro_sorteado(s);
            if (p->pode_virar) p->lado = (int)(proximo(s) % 2);
            ultima = *p;
            if (!cabe_aqui(c, ordem[k], colocada)) continue;
            fio = fio_ate_colocadas(c, ordem[k], colocada) + sorteio(s);
            if (melhor < 0 || fio < melhor) { melhor = fio; melhor_pose = *p; }
        }
        /* sem lugar livre: fica na ultima tentativa, e o recozimento que se vire */
        *p = melhor >= 0 ? melhor_pose : ultima;
        corpo_no_mundo(c, ordem[k]);
        colocada[ordem[k]] = 1;
    }
    free(colocada); free(ordem); free(peso);
}

/* ------------------------------------------------------------------ paralelo */

/* Cada sorteio e independente - posiciona e roteia do zero -, entao mais nucleo e
 * literalmente mais sorteio por segundo. O que eles dividem fica sob a trava: o
 * contador de candidatos, a vencedora e o peso aprendido das redes. */
#ifdef _WIN32
typedef CRITICAL_SECTION Trava;
typedef HANDLE Fio;
static void trava_cria(Trava *t) { InitializeCriticalSection(t); }
static void trava_fecha(Trava *t) { DeleteCriticalSection(t); }
static void tranca(Trava *t) { EnterCriticalSection(t); }
static void destranca(Trava *t) { LeaveCriticalSection(t); }
static double agora(void) { return (double)GetTickCount64() / 1000.0; }
static int nucleos_da_maquina(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
static void trabalha(void *arg);
static DWORD WINAPI ponte(LPVOID a) { trabalha(a); return 0; }
static int fio_cria(Fio *f, void *arg)
{
    *f = CreateThread(NULL, 0, ponte, arg, 0, NULL);
    return *f != NULL;
}
static void fio_espera(Fio f) { WaitForSingleObject(f, INFINITE); CloseHandle(f); }
#else
typedef pthread_mutex_t Trava;
typedef pthread_t Fio;
static void trava_cria(Trava *t) { pthread_mutex_init(t, NULL); }
static void trava_fecha(Trava *t) { pthread_mutex_destroy(t); }
static void tranca(Trava *t) { pthread_mutex_lock(t); }
static void destranca(Trava *t) { pthread_mutex_unlock(t); }
static double agora(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static int nucleos_da_maquina(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
static void trabalha(void *arg);
static void *ponte(void *a) { trabalha(a); return NULL; }
static int fio_cria(Fio *f, void *arg) { return pthread_create(f, NULL, ponte, arg) == 0; }
static void fio_espera(Fio f) { pthread_join(f, NULL); }
#endif

#define PESO_POR_FALHA 0.5
/* Teto baixo de proposito. A rede que falha ganha peso e o sorteio aproxima os
 * pads dela - bom quando falta caminho curto, ruim quando falta ESPACO: no
 * interfone o SCK e o MISO precisam se cruzar entre o J1 e o Nano, e puxar o J1
 * para perto do Nano era justamente tirar o lugar das vias do cruzamento. Quem
 * resolve isso e o sorteio de reparo (ver perturba). */
#define PESO_MAX       3.0

typedef struct {
    Ctx base;                    /* so leitura: geometria, pads, redes */
    const PbPeca *inicial;
    double x0, y0, larg, alt;
    const double *contorno;
    int n_contorno;
    int passos;
    unsigned long long semente;
    int max_candidatos;          /* 0 = sem teto, vale so o prazo */
    double prazo;                /* instante absoluto; <= 0 = sem prazo */

    Trava trava;                 /* tudo daqui para baixo */
    int proximo;
    int acabou;
    int melhor_ab, melhor_vias, vencedor;
    double melhor_fio;
    int melhor_faixa;            /* conectores longe da borda, em faixas de 0,5 mm */
    double melhor_perda;         /* plano perdido, mm2 */
    int sem_melhora;             /* sorteios roteados desde a ultima melhora */
    PbPeca *pose_vencedora;
    /* pecas com pad nas ligacoes que a vencedora deixou abertas: e nelas que o
     * sorteio de reparo mexe */
    unsigned char *culpadas;
    /* Busca sistematica: poses a testar para as culpadas da vencedora, uma de
     * cada vez, partindo dela. Refeita a cada vencedora nova. */
    struct Pose { int peca; double x, y, giro; int lado; } *lista;
    int n_lista, prox_lista, cap_lista;
    int n_culpadas;
    double *peso;                /* por rede: quem nao fecha pesa mais no custo */
    int descartados, julgados;
    PbPlacar *placar;            /* publicado ao vivo; `parar` vem de quem chamou */
} Sorteio;

/* O juiz. A ordem e a do projeto: FECHAR vem antes de tudo - jumper e proibido,
 * ligacao aberta e defeito. Entre posicoes que fecham igual, ganha a que poe os
 * conectores na periferia; depois, menos via; depois, menos fio.
 *
 * Faltava o conector aqui: ele so pesava no custo do recozimento, e o juiz parava
 * no primeiro candidato que fechasse tudo. Placa que ja chegava fechada (a que foi
 * fresada) devolvia a propria entrada, com J1 no meio. */
#define FAIXA_MM        0.5   /* conector por dentro, comparado de meio em meio mm */
#define FAIXA_ENCOSTADO 0     /* todo conector na periferia: pode parar */

static int faixa_conectores(const Ctx *c)
{
    return (int)floor(conectores_por_dentro(c) / FAIXA_MM);
}

#define PERDA_IGUAL     1.0   /* mm2 de plano: menos que isto e empate */
#define PACIENCIA       300   /* com plano: sorteios sem melhora antes de parar */

static int melhor_que(int ab, int faixa, double perda, int nv, double fio,
                      const Sorteio *g)
{
    if (ab != g->melhor_ab) return ab < g->melhor_ab;
    if (faixa != g->melhor_faixa) return faixa < g->melhor_faixa;
    if (fabs(perda - g->melhor_perda) > PERDA_IGUAL) return perda < g->melhor_perda;
    if (nv != g->melhor_vias) return nv < g->melhor_vias;
    return fio < g->melhor_fio - 1e-6;
}

/* Busca sistematica em volta das culpadas.
 *
 * O salto aleatorio (perturba) sorteia lugar na placa inteira: mais da metade cai
 * em pose ilegal, e a regiao que desfaz o cruzamento - o J1 junto da ponta de
 * baixo do Nano, no interfone - e pequena demais para ser acertada assim. Aqui
 * cada culpada passa por uma grade de 2,5 mm num raio de 15 mm em volta de onde
 * esta, nos 8 giros de 45 em 45 (e nos 2 lados, se pode virar), uma pose por vez, o resto da
 * vencedora parado. Embaralhada, para os nucleos varrerem a regiao toda por
 * igual em vez de esgotar um canto primeiro. */
#define SIST_RAIO   15.0
#define SIST_PASSO   2.5

static void monta_lista(Sorteio *g, const Ctx *c, unsigned long long *s)
{
    int i, n = 0, passos = (int)(SIST_RAIO / SIST_PASSO);
    g->n_lista = 0;
    g->prox_lista = 0;
    for (i = 0; i < c->n_pecas; i++) {
        const PbPeca *p = &c->pecas[i];
        int dx, dy, gi, la;
        if (!g->culpadas[i] || !p->movel) continue;
        for (dy = -passos; dy <= passos; dy++)
            for (dx = -passos; dx <= passos; dx++) {
                if (dx * dx + dy * dy > passos * passos) continue;
                for (gi = 0; gi < 8; gi++)          /* de 45 em 45 */
                    for (la = 0; la < (p->pode_virar ? 2 : 1); la++) {
                        struct Pose *q;
                        if (n == g->cap_lista) {
                            int cap = g->cap_lista ? g->cap_lista * 2 : 1024;
                            struct Pose *novo = (struct Pose *)realloc(g->lista,
                                                    (size_t)cap * sizeof(struct Pose));
                            if (!novo) goto pronto;
                            g->lista = novo;
                            g->cap_lista = cap;
                        }
                        q = &g->lista[n++];
                        q->peca = i;
                        q->x = arredonda(p->x + dx * SIST_PASSO);
                        q->y = arredonda(p->y + dy * SIST_PASSO);
                        q->giro = 45.0 * gi;
                        q->lado = p->pode_virar ? la : p->lado;
                    }
            }
    }
pronto:
    for (i = n - 1; i > 0; i--) {           /* embaralha */
        int j = (int)(proximo(s) % (unsigned long long)(i + 1));
        struct Pose t = g->lista[i];
        g->lista[i] = g->lista[j];
        g->lista[j] = t;
    }
    g->n_lista = n;
}

static int sorteio_continua(const Sorteio *g)
{
    if (g->acabou || g->placar->parar) return 0;
    if (g->max_candidatos > 0 && g->proximo > g->max_candidatos) return 0;
    if (g->prazo > 0 && agora() > g->prazo) return 0;
    return 1;
}

/* Um nucleo: pega o proximo numero de candidato, recoze a partir da posicao de
 * entrada, descarta se for ilegal, roteia, e conta no placar comum. Para quando
 * alguem fechou a placa inteira, quando acabou o prazo ou o teto de candidatos. */
static void trabalha(void *arg)
{
    Sorteio *g = (Sorteio *)arg;
    Ctx c = g->base;
    int nr = c.n_redes > 0 ? c.n_redes : 1, i;
    PbPeca *pecas = (PbPeca *)malloc((size_t)c.n_pecas * sizeof(PbPeca));
    double *peso = (double *)malloc((size_t)nr * sizeof(double));
    int *falhas = (int *)malloc((size_t)nr * sizeof(int));
    PbMapa *mapa = NULL;          /* deste nucleo, reaproveitado (ver julga) */
    unsigned char *culpadas = (unsigned char *)malloc((size_t)(c.n_pecas > 0 ? c.n_pecas : 1));
    unsigned char *movidas = (unsigned char *)malloc((size_t)(c.n_pecas > 0 ? c.n_pecas : 1));
    c.pecas = pecas;
    c.peso = peso;
    c.mundo = (double *)malloc((size_t)c.n_pecas * MAX_CASCO * 2 * sizeof(double));
    c.caixas = (double *)malloc((size_t)c.n_pecas * 4 * sizeof(double));

    for (;;) {
        int k, ab, nv, reparo, sistematico;
        double fio, perda;
        unsigned long long s;
        int faixa, perde;

        tranca(&g->trava);
        if (!sorteio_continua(g)) { destranca(&g->trava); break; }
        k = g->proximo++;
        memcpy(peso, g->peso, (size_t)nr * sizeof(double));
        /* metade dos sorteios, enquanto sobrar ligacao aberta, e de REPARO:
         * parte da melhor posicao e mexe so nas pecas culpadas */
        sistematico = g->melhor_ab > 0 && g->prox_lista < g->n_lista;
        if (sistematico) {
            /* a vencedora com UMA peca culpada numa pose da grade */
            const struct Pose *q = &g->lista[g->prox_lista++];
            memcpy(pecas, g->pose_vencedora, (size_t)c.n_pecas * sizeof(PbPeca));
            pecas[q->peca].x = q->x; pecas[q->peca].y = q->y;
            pecas[q->peca].giro = q->giro; pecas[q->peca].lado = q->lado;
            reparo = 0;
        } else {
            reparo = g->melhor_ab > 0 && g->n_culpadas > 0 && (k % 2 == 1);
            if (reparo) {
                memcpy(pecas, g->pose_vencedora, (size_t)c.n_pecas * sizeof(PbPeca));
                memcpy(culpadas, g->culpadas, (size_t)c.n_pecas);
            }
        }
        destranca(&g->trava);

        if (!reparo && !sistematico)
            memcpy(pecas, g->inicial, (size_t)c.n_pecas * sizeof(PbPeca));
        s = g->semente ^ ((unsigned long long)k * 0x9E3779B97F4A7C15ULL);
        if (!s) s = 1;
        proximo(&s);
        if (sistematico) {
            /* nada a recozer: a pose e exatamente a da grade (ilegal sai ja) */
        } else if (!reparo && (k % 3 == 2 || (g->julgados == 0 && k > 30))) {
            /* do zero: a posicao de entrada nao conta; depois, acabamento morno */
            monta_do_zero(&c, &s);
            recoze(&c, g->passos, &s, 0.3, NULL);
        } else if (reparo) {
            /* a peca que saltou fica CONGELADA no lugar novo: o recozimento, que
             * so enxerga fio, a puxava de volta para o mesmo canal apertado */
            perturba(&c, culpadas, &s, movidas);
            recoze(&c, g->passos / 2, &s, 0.05, movidas);
        } else {
            recoze(&c, g->passos, &s, 1.0, NULL);
        }
        if (!legal(&c)) {
            tranca(&g->trava);
            g->descartados++;
            g->placar->descartados = g->descartados;
            destranca(&g->trava);
            continue;
        }
        /* legal() acabou de levar os corpos para o mundo: a medida e desta pose */
        faixa = faixa_conectores(&c);
        tranca(&g->trava);
        perde = g->melhor_ab == 0 && faixa > g->melhor_faixa;
        destranca(&g->trava);
        if (perde) continue;     /* nem fechando tudo ganharia: nao vale rotear */
        memset(falhas, 0, (size_t)nr * sizeof(int));
        ab = julga(&c, g->x0, g->y0, g->larg, g->alt, g->contorno, g->n_contorno,
                   &nv, &fio, falhas, &perda, &mapa);
        if (g->placar->parar) break;    /* roteamento interrompido: nao vale */

        tranca(&g->trava);
        g->julgados++;
        /* a rede que nao fechou ganha peso: os proximos sorteios aproximam os
         * pads dela, como a perfboard fazia entre uma tentativa e outra */
        for (i = 0; i < c.n_redes; i++)
            if (falhas[i]) {
                g->peso[i] += PESO_POR_FALHA * falhas[i];
                if (g->peso[i] > PESO_MAX) g->peso[i] = PESO_MAX;
            }
        g->sem_melhora++;
        if (melhor_que(ab, faixa, perda, nv, fio, g)) {
            g->melhor_ab = ab; g->melhor_vias = nv; g->melhor_fio = fio;
            g->melhor_faixa = faixa;
            g->melhor_perda = perda;
            g->sem_melhora = 0;
            g->vencedor = k;
            memcpy(g->pose_vencedora, pecas, (size_t)c.n_pecas * sizeof(PbPeca));
            marca_culpadas(&c, falhas, g->culpadas, &g->n_culpadas);
            if (ab > 0) monta_lista(g, &c, &s);
        }
        /* sem plano, fechou e conector na periferia: pronto. Com plano, ainda
         * da para perder menos plano - segue ate parar de melhorar */
        if (g->melhor_ab == 0 && g->melhor_faixa <= FAIXA_ENCOSTADO
                && (g->base.n_planos == 0 || g->sem_melhora >= PACIENCIA))
            g->acabou = 1;
        /* placar ao vivo: a janela do KiCad le enquanto o sorteio corre */
        g->placar->julgados = g->julgados;
        g->placar->abertas_depois = g->melhor_ab;
        g->placar->vias = g->melhor_vias;
        g->placar->vencedor = g->vencedor;
        g->placar->conectores_mm = g->melhor_faixa * FAIXA_MM;
        g->placar->plano_perdido = g->melhor_perda;
        destranca(&g->trava);
    }

    free(pecas); free(peso); free(falhas); free(c.mundo); free(c.caixas);
    pb_destroi(mapa);
    free(culpadas);
    free(movidas);
}

/* ------------------------------------------------------------------ api */

int pb_posiciona(const PbRegras *r,
                 double x0, double y0, double larg, double alt,
                 const double *contorno, int n_contorno,
                 const PbProibida *proibidas, int n_proibidas,
                 const PbPlano *planos, int n_planos,
                 PbPeca *pecas, int n_pecas,
                 const PbPadC *pads, int n_pads,
                 int candidatos, int passos, unsigned long long semente,
                 double segundos, int nucleos,
                 PbPlacar *placar)
{
    Sorteio g;
    Ctx *c = &g.base;
    PbRegras juiz;
    Fio *fios;
    int i, n_fios = 0, nr, *falhas, *n_casco;
    double t0 = agora();

    memset(&g, 0, sizeof(g));
    /* O juiz roteia CENTENAS de posicoes: so precisa ordenar uma contra a outra,
     * nao entregar o melhor roteamento. Vai com busca mais direta (estimativa
     * x2) e ate 3 passadas; a vencedora e roteada com capricho depois. */
    juiz = *r;
    if (juiz.peso_estimativa < 2.0) juiz.peso_estimativa = 2.0;
    if (juiz.tentativas > 3) juiz.tentativas = 3;
    c->r = &juiz;
    c->proibidas = proibidas; c->n_proibidas = n_proibidas;
    c->planos = planos; c->n_planos = n_planos;
    c->parar = &placar->parar;
    c->pecas = pecas; c->n_pecas = n_pecas;
    c->pads = pads; c->n_pads = n_pads;

    if (n_contorno >= 3) {
        c->bx0 = c->bx1 = contorno[0];
        c->by0 = c->by1 = contorno[1];
        for (i = 1; i < n_contorno; i++) {
            double x = contorno[i * 2], y = contorno[i * 2 + 1];
            if (x < c->bx0) c->bx0 = x;
            if (x > c->bx1) c->bx1 = x;
            if (y < c->by0) c->by0 = y;
            if (y > c->by1) c->by1 = y;
        }
    } else {
        c->bx0 = x0; c->by0 = y0; c->bx1 = x0 + larg; c->by1 = y0 + alt;
    }

    c->redes = (int *)malloc((size_t)(n_pads > 0 ? n_pads : 1) * sizeof(int));
    for (i = 0; i < n_pads; i++) {
        int j, ja = 0;
        if (!pads[i].rede) continue;
        for (j = 0; j < c->n_redes; j++) if (c->redes[j] == pads[i].rede) { ja = 1; break; }
        if (!ja) c->redes[c->n_redes++] = pads[i].rede;
    }
    nr = c->n_redes > 0 ? c->n_redes : 1;

    /* casco de cada peca; sem courtyard (ou com courtyard grande demais para o
     * recorte), a caixa que veio em larg/alt/cx/cy */
    c->casco = (double *)calloc((size_t)(n_pecas > 0 ? n_pecas : 1) * MAX_CASCO * 2,
                                sizeof(double));
    n_casco = (int *)calloc((size_t)(n_pecas > 0 ? n_pecas : 1), sizeof(int));
    c->n_casco = n_casco;
    for (i = 0; i < n_pecas; i++) {
        double *dst = &c->casco[i * MAX_CASCO * 2];
        const PbPeca *pc = &pecas[i];
        int n = 0;
        if (pc->n_corpo >= 3) {
            double *tmp = (double *)malloc((size_t)pc->n_corpo * 2 * sizeof(double));
            double *h = (double *)malloc((size_t)(pc->n_corpo * 2 + 2) * 2 * sizeof(double));
            memcpy(tmp, pc->corpo, (size_t)pc->n_corpo * 2 * sizeof(double));
            n = casco_convexo(tmp, pc->n_corpo, h);
            if (n >= 3 && n <= MAX_CASCO) memcpy(dst, h, (size_t)n * 2 * sizeof(double));
            else n = 0;
            free(tmp); free(h);
        }
        if (n == 0) {
            double hx = pc->larg / 2.0, hy = pc->alt / 2.0;
            dst[0] = pc->cx - hx; dst[1] = pc->cy - hy;
            dst[2] = pc->cx + hx; dst[3] = pc->cy - hy;
            dst[4] = pc->cx + hx; dst[5] = pc->cy + hy;
            dst[6] = pc->cx - hx; dst[7] = pc->cy + hy;
            n = 4;
        }
        n_casco[i] = n;
    }
    c->mundo = (double *)malloc((size_t)(n_pecas > 0 ? n_pecas : 1) * MAX_CASCO * 2
                                * sizeof(double));
    c->caixas = (double *)malloc((size_t)(n_pecas > 0 ? n_pecas : 1) * 4 * sizeof(double));

    g.peso = (double *)malloc((size_t)nr * sizeof(double));
    for (i = 0; i < nr; i++) g.peso[i] = 1.0;
    falhas = (int *)calloc((size_t)nr, sizeof(int));

    g.inicial = (PbPeca *)malloc((size_t)n_pecas * sizeof(PbPeca));
    g.pose_vencedora = (PbPeca *)malloc((size_t)n_pecas * sizeof(PbPeca));
    g.culpadas = (unsigned char *)calloc((size_t)(n_pecas > 0 ? n_pecas : 1), 1);
    memcpy((PbPeca *)g.inicial, pecas, (size_t)n_pecas * sizeof(PbPeca));
    memcpy(g.pose_vencedora, pecas, (size_t)n_pecas * sizeof(PbPeca));
    g.x0 = x0; g.y0 = y0; g.larg = larg; g.alt = alt;
    g.contorno = contorno; g.n_contorno = n_contorno;
    g.passos = passos;
    g.semente = semente ? semente : 0x9E3779B97F4A7C15ULL;
    g.max_candidatos = candidatos;
    g.prazo = segundos > 0 ? t0 + segundos : 0.0;
    if (g.max_candidatos <= 0 && g.prazo <= 0) g.max_candidatos = 24;  /* nunca sem fim */

    /* candidato 0: a posicao de entrada, intocada - e as redes dela que nao
     * fecham ja comecam com peso */
    g.melhor_ab = julga(c, x0, y0, larg, alt, contorno, n_contorno,
                        &g.melhor_vias, &g.melhor_fio, falhas, &g.melhor_perda, NULL);
    placar->plano_perdido = g.melhor_perda;
    corpos_no_mundo(c);
    g.melhor_faixa = faixa_conectores(c);
    placar->conectores_mm = g.melhor_faixa * FAIXA_MM;
    for (i = 0; i < c->n_redes; i++)
        if (falhas[i]) g.peso[i] += PESO_POR_FALHA * falhas[i];
    marca_culpadas(c, falhas, g.culpadas, &g.n_culpadas);
    if (g.melhor_ab > 0) {
        unsigned long long s0 = g.semente ^ 0x5DEECE66DULL;
        c->pecas = g.pose_vencedora;
        monta_lista(&g, c, &s0);
        c->pecas = pecas;
    }
    g.placar = placar;
    placar->abertas_antes = placar->abertas_depois = g.melhor_ab;
    placar->vias = g.melhor_vias;
    placar->julgados = placar->descartados = placar->vencedor = 0;
    g.vencedor = 0;
    g.proximo = 1;
    g.acabou = (g.melhor_ab == 0 && g.melhor_faixa <= FAIXA_ENCOSTADO
                && n_planos == 0);

    /* Padrao: METADE dos nucleos. Com todos, o computador inteiro trava enquanto
     * o sorteio roda - e quem esta usando o KiCad precisa da maquina. */
    if (nucleos <= 0) nucleos = nucleos_da_maquina() / 2;
    if (nucleos < 1) nucleos = 1;
    fios = (Fio *)malloc((size_t)nucleos * sizeof(Fio));
    trava_cria(&g.trava);
    for (i = 0; i < nucleos; i++)
        if (fio_cria(&fios[n_fios], &g)) n_fios++;
    if (n_fios == 0) trabalha(&g);          /* sem thread: roda aqui mesmo */
    for (i = 0; i < n_fios; i++) fio_espera(fios[i]);
    trava_fecha(&g.trava);

    memcpy(pecas, g.pose_vencedora, (size_t)n_pecas * sizeof(PbPeca));
    placar->abertas_depois = g.melhor_ab;
    placar->vias = g.melhor_vias;
    placar->fio = g.melhor_fio;
    placar->vencedor = g.vencedor;
    placar->descartados = g.descartados;
    placar->julgados = g.julgados;
    placar->nucleos = n_fios > 0 ? n_fios : 1;
    placar->segundos = agora() - t0;

    free(fios); free(falhas); free(g.peso);
    free((PbPeca *)g.inicial); free(g.pose_vencedora); free(c->redes);
    free(g.culpadas);
    free(g.lista);
    free(c->casco); free(n_casco); free(c->mundo); free(c->caixas);
    return g.melhor_ab;
}
