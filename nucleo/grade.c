/* Rasterizacao: a placa vira grade, e cada obstaculo e pintado JA INFLADO.
 *
 * A ideia que faz o roteamento ficar simples e nao perguntar "ha espaco aqui?"
 * durante a busca. Cada obstaculo entra na grade inflado de meia trilha mais a
 * folga; depois disso a pergunta vira trivial: o centro da trilha pode ocupar
 * qualquer celula que nao esteja pintada contra ele.
 *
 * Celula disputada por duas redes vira PB_DE_NINGUEM em vez de guardar a ultima
 * que pintou. Guardar a ultima abriria passagem para a primeira exatamente onde
 * nao cabe, e o curto sairia na fresa, nao na tela.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "perfcnc.h"
#include "interno.h"

static int indice(const PbMapa *m, int c, int l, int face)
{
    return (l * m->colunas + c) * 2 + face;
}

int pb_colunas(const PbMapa *m) { return m->colunas; }
int pb_linhas(const PbMapa *m) { return m->linhas; }

int pb_coluna_de(const PbMapa *m, double x)
{
    return (int)floor((x - m->x0) / m->regras.grade + 0.5);
}

int pb_linha_de(const PbMapa *m, double y)
{
    return (int)floor((y - m->y0) / m->regras.grade + 0.5);
}

double pb_x_de(const PbMapa *m, int c) { return m->x0 + c * m->regras.grade; }
double pb_y_de(const PbMapa *m, int l) { return m->y0 + l * m->regras.grade; }

int pb_dono(const PbMapa *m, int c, int l, int face)
{
    if (c < 0 || l < 0 || c >= m->colunas || l >= m->linhas)
        return PB_DE_NINGUEM;
    return m->celulas[indice(m, c, l, face)];
}

int pb_livre_para(const PbMapa *m, int c, int l, int face, int rede)
{
    int d = pb_dono(m, c, l, face);
    return d == PB_LIVRE || d == rede;
}

static void marca(PbMapa *m, int c, int l, int face, int rede)
{
    int i, atual;
    if (c < 0 || l < 0 || c >= m->colunas || l >= m->linhas)
        return;
    i = indice(m, c, l, face);
    atual = m->celulas[i];
    if (atual == rede || atual == PB_DE_NINGUEM)
        return;
    m->celulas[i] = (atual == PB_LIVRE) ? rede : PB_DE_NINGUEM;
}

/* ------------------------------------------------------------------ formas */

static void circulo(PbMapa *m, double x, double y, double raio, int face, int rede)
{
    int c0 = pb_coluna_de(m, x - raio), c1 = pb_coluna_de(m, x + raio);
    int l0 = pb_linha_de(m, y - raio), l1 = pb_linha_de(m, y + raio);
    double r2 = raio * raio;
    int c, l;
    for (l = l0; l <= l1; l++) {
        double dy = pb_y_de(m, l) - y;
        for (c = c0; c <= c1; c++) {
            double dx = pb_x_de(m, c) - x;
            if (dx * dx + dy * dy <= r2)
                marca(m, c, l, face, rede);
        }
    }
}

static void retangulo(PbMapa *m, double x, double y, double larg, double alt,
                      double giro, int face, int rede)
{
    double meia = sqrt(larg * larg + alt * alt) / 2.0;
    int c0 = pb_coluna_de(m, x - meia), c1 = pb_coluna_de(m, x + meia);
    int l0 = pb_linha_de(m, y - meia), l1 = pb_linha_de(m, y + meia);
    double t = giro * M_PI / 180.0;      /* mundo -> local: +giro (KiCad, Y para baixo) */
    double ct = cos(t), st = sin(t), hx = larg / 2.0, hy = alt / 2.0;
    int c, l;
    for (l = l0; l <= l1; l++) {
        double dy = pb_y_de(m, l) - y;
        for (c = c0; c <= c1; c++) {
            double dx = pb_x_de(m, c) - x;
            double lx = dx * ct - dy * st;
            double ly = dx * st + dy * ct;
            if (fabs(lx) <= hx && fabs(ly) <= hy)
                marca(m, c, l, face, rede);
        }
    }
}

/* Oval do KiCad: retangulo com as duas pontas arredondadas. */
static void capsula(PbMapa *m, double x, double y, double larg, double alt,
                    double giro, int face, int rede)
{
    double t = -giro * M_PI / 180.0;     /* eixo da capsula no mundo: Rot(-giro) */
    double raio, meio, dx, dy;
    if (larg >= alt) {
        raio = alt / 2.0;
        meio = (larg - alt) / 2.0;
        dx = cos(t) * meio;
        dy = sin(t) * meio;
    } else {
        raio = larg / 2.0;
        meio = (alt - larg) / 2.0;
        dx = -sin(t) * meio;
        dy = cos(t) * meio;
    }
    circulo(m, x - dx, y - dy, raio, face, rede);
    circulo(m, x + dx, y + dy, raio, face, rede);
    if (larg >= alt)
        retangulo(m, x, y, larg - alt, alt, giro, face, rede);
    else
        retangulo(m, x, y, larg, alt - larg, giro, face, rede);
}

static void segmento(PbMapa *m, double xa, double ya, double xb, double yb,
                     double raio, int face, int rede)
{
    double comp = hypot(xb - xa, yb - ya);
    int passos = (int)(comp / (m->regras.grade / 2.0));
    int i;
    if (passos < 1) passos = 1;
    for (i = 0; i <= passos; i++) {
        double t = (double)i / passos;
        circulo(m, xa + (xb - xa) * t, ya + (yb - ya) * t, raio, face, rede);
    }
}

static int dentro(double x, double y, const double *p, int n)
{
    int dentro = 0, i;
    for (i = 0; i < n; i++) {
        double x1 = p[i * 2], y1 = p[i * 2 + 1];
        double x2 = p[((i + 1) % n) * 2], y2 = p[((i + 1) % n) * 2 + 1];
        if ((y1 > y) != (y2 > y)) {
            double corte = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
            if (x < corte) dentro = !dentro;
        }
    }
    return dentro;
}

static double ate_a_borda(double x, double y, const double *p, int n)
{
    double melhor = 1e30;
    int i;
    for (i = 0; i < n; i++) {
        double x1 = p[i * 2], y1 = p[i * 2 + 1];
        double x2 = p[((i + 1) % n) * 2], y2 = p[((i + 1) % n) * 2 + 1];
        double dx = x2 - x1, dy = y2 - y1, t, px, py, d;
        if (dx == 0.0 && dy == 0.0) {
            d = hypot(x - x1, y - y1);
        } else {
            t = ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            px = x1 + t * dx;
            py = y1 + t * dy;
            d = hypot(x - px, y - py);
        }
        if (d < melhor) melhor = d;
    }
    return melhor;
}

/* A folga de poligono e sempre DISTANCIA ATE A BORDA, nunca poligono inflado.
 * Inflar poligono concavo na mao erra nos cantos reentrantes, e o erro apareceria
 * como trilha encostando numa area proibida - visivel so depois de fresar. */
static void poligono(PbMapa *m, const double *p, int n, double folga,
                     int face, int rede, int inverte)
{
    int c, l;
    for (l = 0; l < m->linhas; l++) {
        double y = pb_y_de(m, l);
        for (c = 0; c < m->colunas; c++) {
            double x = pb_x_de(m, c);
            int d = dentro(x, y, p, n);
            int bate = inverte ? (!d || ate_a_borda(x, y, p, n) <= folga)
                               : (d || ate_a_borda(x, y, p, n) <= folga);
            if (bate) marca(m, c, l, face, rede);
        }
    }
}

/* ------------------------------------------------------------------ api */

PbMapa *pb_cria(const PbRegras *regras, double x0, double y0,
                double largura, double altura)
{
    PbMapa *m = (PbMapa *)calloc(1, sizeof(PbMapa));
    if (!m) return NULL;
    m->regras = *regras;
    m->x0 = x0;
    m->y0 = y0;
    m->colunas = (int)ceil(largura / regras->grade) + 1;
    m->linhas = (int)ceil(altura / regras->grade) + 1;
    if (m->colunas < 1) m->colunas = 1;
    if (m->linhas < 1) m->linhas = 1;
    m->celulas = (int *)calloc((size_t)m->colunas * m->linhas * 2, sizeof(int));
    if (!m->celulas) { free(m); return NULL; }
    return m;
}

void pb_destroi(PbMapa *m)
{
    if (!m) return;
    free(m->celulas);
    free(m->soma[0]);
    free(m->soma[1]);
    free(m);
}

void pb_pinta(PbMapa *m, const PbForma *formas, int quantas)
{
    double base = m->regras.trilha / 2.0 + m->regras.folga;
    int i, face;
    for (i = 0; i < quantas; i++) {
        const PbForma *f = &formas[i];
        double folga = base + f->folga_extra;
        for (face = 0; face < 2; face++) {
            int bit = (face == 0) ? PB_CIMA : PB_BAIXO;
            if (!(f->faces & bit)) continue;
            switch (f->forma) {
            case PB_CIRCULO:
                circulo(m, f->x, f->y, f->larg / 2.0 + folga, face, f->rede);
                break;
            case PB_CAPSULA:
                capsula(m, f->x, f->y, f->larg + 2 * folga, f->alt + 2 * folga,
                        f->giro, face, f->rede);
                break;
            case PB_RETANGULO:
                retangulo(m, f->x, f->y, f->larg + 2 * folga, f->alt + 2 * folga,
                          f->giro, face, f->rede);
                break;
            case PB_SEGMENTO:
                segmento(m, f->x, f->y, f->larg, f->alt,
                         f->giro / 2.0 + folga, face, f->rede);
                break;
            case PB_POLIGONO:
                poligono(m, f->pontos, f->n_pontos, folga, face, f->rede, 0);
                break;
            case PB_FORA_DE:
                poligono(m, f->pontos, f->n_pontos, folga, face, f->rede, 1);
                break;
            default:
                break;
            }
        }
    }
    /* o mapa mudou: o que o roteador sabia sobre onde cabe via envelheceu */
    m->soma_da_rede = -999999;
}

int pb_abi(void) { return PB_ABI; }
