#pragma once
// Tela touch da ESP32-2432S028 (opcional, CONFIG_PICTIONARY_TELA).
// O main.cc continua dono dos modelos: a tela só pede um palpite por esta função.
#include <stdint.h>

struct TelaPalpite {
    int classe;   // índice em kCategorias
    float prob;   // 0..1
    float ms;     // tempo do Invoke()
};

// recebe o desenho 28x28 e preenche o palpite do float32 (res[0]) e do int8 (res[1])
typedef void (*TelaPrever)(const uint8_t* pixels, TelaPalpite res[2]);

void tela_iniciar(TelaPrever prever);
void tela_status(const char* texto);  // linha de status (ex.: o IP); pode ser chamada de qualquer task
