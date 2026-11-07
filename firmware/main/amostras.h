// Gerado por training/exportar_c.py -- nao edite a mao.
#pragma once
#include <stdint.h>

#define NUM_CLASSES 15
#define AMOSTRAS_POR_CLASSE 20
#define NUM_AMOSTRAS 300
#define IMG_PIXELS 784

static const char* const kCategorias[NUM_CLASSES] = {"apple", "bicycle", "car", "cat", "clock", "eye", "fish", "house", "key", "light bulb", "rainbow", "star", "sun", "tree", "umbrella"};

// desenhos 28x28 em uint8 (0..255), um apos o outro
extern const uint8_t amostras_img[];
extern const uint8_t amostras_rotulo[];
