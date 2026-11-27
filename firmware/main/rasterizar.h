#pragma once
// Pré-processamento do desenho da tela touch: a mesma receita do index.html, em C.
// Não depende do ESP-IDF, então dá para compilar e testar no PC.
#include <stdint.h>

struct Ponto {
    int16_t x, y;
};

// pontos: todos os pontos, traço após traço
// inicio[k]: índice do primeiro ponto do traço k (o traço vai até inicio[k+1] ou n_pontos)
// saida: 28x28 uint8, traço branco (255) em fundo preto (0), como o Quick, Draw!
// Devolve false se faltar memória para a imagem intermediária.
bool rasterizar(const Ponto* pontos, int n_pontos, const uint16_t* inicio, int n_tracos,
                uint8_t saida[28 * 28]);
