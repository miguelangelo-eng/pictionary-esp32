#include "rasterizar.h"

#include <stdlib.h>
#include <string.h>

// Mesmos números do preprocessar() do index.html, que trabalha numa tela de 256x256:
//   margem de 10 px, traço de 16 px, depois reduz para 28x28 com bordas suaves.
// Aqui o desenho é feito direto numa grade de 224x224 (8x8 subpixels por pixel final)
// em 1 bit por subpixel (6 KB), e cada pixel final é a fração de subpixels pintados.
// O resultado é o mesmo "traço com borda suave" que o navegador gera.
static constexpr int SAIDA = 28;
static constexpr int SUB = 8;                       // subpixels por pixel, em cada eixo
static constexpr int GRADE = SAIDA * SUB;           // 224
static constexpr float TAM = 256.0f;                // tamanho da tela do navegador
static constexpr float MARGEM = 10.0f;
static constexpr float ESPESSURA = 16.0f;
static constexpr float FATOR = GRADE / TAM;         // 256 -> 224
static constexpr int BYTES_LINHA = GRADE / 8;

static inline void pintar(uint8_t* grade, int x, int y) {
    grade[y * BYTES_LINHA + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

// pinta todos os subpixels a até 'r' do segmento (ax,ay)-(bx,by): linha com pontas redondas
static void segmento(uint8_t* grade, float ax, float ay, float bx, float by, float r) {
    float minx = (ax < bx ? ax : bx) - r, maxx = (ax > bx ? ax : bx) + r;
    float miny = (ay < by ? ay : by) - r, maxy = (ay > by ? ay : by) + r;
    int x0 = (int)minx, x1 = (int)maxx + 1, y0 = (int)miny, y1 = (int)maxy + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GRADE - 1) x1 = GRADE - 1;
    if (y1 > GRADE - 1) y1 = GRADE - 1;

    float vx = bx - ax, vy = by - ay;
    float comp2 = vx * vx + vy * vy;
    float r2 = r * r;
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;                        // centro do subpixel
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float t = comp2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / comp2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
            if (dx * dx + dy * dy <= r2) pintar(grade, x, y);
        }
    }
}

bool rasterizar(const Ponto* pontos, int n_pontos, const uint16_t* inicio, int n_tracos,
                uint8_t saida[SAIDA * SAIDA]) {
    memset(saida, 0, SAIDA * SAIDA);
    if (n_pontos <= 0 || n_tracos <= 0) return true;

    // 1. caixa em volta de todos os traços
    int x0 = pontos[0].x, y0 = pontos[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < n_pontos; i++) {
        if (pontos[i].x < x0) x0 = pontos[i].x;
        if (pontos[i].y < y0) y0 = pontos[i].y;
        if (pontos[i].x > x1) x1 = pontos[i].x;
        if (pontos[i].y > y1) y1 = pontos[i].y;
    }

    // 2 e 3. escala única (mantém a proporção) e centraliza, em coordenadas 256x256
    float largura = (x1 - x0) > 1 ? (float)(x1 - x0) : 1.0f;
    float altura = (y1 - y0) > 1 ? (float)(y1 - y0) : 1.0f;
    float maior = largura > altura ? largura : altura;
    float escala = (TAM - 2 * MARGEM) / maior;
    float dx = (TAM - largura * escala) / 2 - x0 * escala;
    float dy = (TAM - altura * escala) / 2 - y0 * escala;

    // já convertido para a grade de 224
    escala *= FATOR;
    dx *= FATOR;
    dy *= FATOR;
    const float raio = ESPESSURA / 2 * FATOR;

    uint8_t* grade = (uint8_t*)calloc(GRADE * BYTES_LINHA, 1);
    if (grade == nullptr) return false;

    // 4. redesenha os traços (branco sobre preto)
    for (int k = 0; k < n_tracos; k++) {
        int ini = inicio[k];
        int fim = (k + 1 < n_tracos) ? inicio[k + 1] : n_pontos;
        if (ini >= fim) continue;
        float ax = pontos[ini].x * escala + dx, ay = pontos[ini].y * escala + dy;
        if (fim - ini == 1) {                       // um toque só vira um ponto
            segmento(grade, ax, ay, ax, ay, raio);
            continue;
        }
        for (int i = ini + 1; i < fim; i++) {
            float bx = pontos[i].x * escala + dx, by = pontos[i].y * escala + dy;
            segmento(grade, ax, ay, bx, by, raio);
            ax = bx;
            ay = by;
        }
    }

    // 5. reduz para 28x28: cada pixel é a média dos seus 8x8 subpixels
    for (int py = 0; py < SAIDA; py++) {
        for (int px = 0; px < SAIDA; px++) {
            int soma = 0;
            for (int sy = 0; sy < SUB; sy++) {
                uint8_t b = grade[(py * SUB + sy) * BYTES_LINHA + px];  // 8 subpixels = 1 byte
                soma += __builtin_popcount(b);
            }
            saida[py * SAIDA + px] = (uint8_t)((soma * 255 + SUB * SUB / 2) / (SUB * SUB));
        }
    }

    free(grade);
    return true;
}
