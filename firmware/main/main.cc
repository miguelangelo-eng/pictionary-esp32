#include <stdio.h>
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_system.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

#include "amostras.h"
#include "modelo_float32.h"
#include "modelo_int8.h"

// espaço de trabalho do modelo; o valor real usado é impresso no monitor
constexpr size_t kArenaSize = 64 * 1024;

extern "C" void app_main(void)
{
    printf("\n=== Pictionary ESP32: primeira inferencia ===\n");
    printf("RAM livre no inicio: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

    // 1. "abre" o modelo direto da flash (nada é copiado para a RAM)
    const tflite::Model* modelo = tflite::GetModel(modelo_int8);
    if (modelo->version() != TFLITE_SCHEMA_VERSION) {
        printf("ERRO: schema do modelo (%lu) diferente do esperado (%d)\n",
               (unsigned long)modelo->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    // 2. registra as 5 operações que o modelo usa (aquela lista da etapa 3)
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddConv2D();
    resolver.AddMaxPool2D();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    // 3. reserva a tensor arena na RAM interna
    uint8_t* arena = (uint8_t*)heap_caps_malloc(kArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (arena == nullptr) {
        printf("ERRO: sem RAM para a arena de %u bytes\n", (unsigned)kArenaSize);
        return;
    }

    // 4. cria o interpretador e distribui os tensores dentro da arena
    static tflite::MicroInterpreter interpretador(modelo, resolver, arena, kArenaSize);
    if (interpretador.AllocateTensors() != kTfLiteOk) {
        printf("ERRO: AllocateTensors falhou (arena pequena demais?)\n");
        return;
    }
    printf("Arena: usou %u de %u bytes\n",
           (unsigned)interpretador.arena_used_bytes(), (unsigned)kArenaSize);

    TfLiteTensor* entrada = interpretador.input(0);
    TfLiteTensor* saida = interpretador.output(0);
    printf("Entrada: scale=%f zero_point=%d\n", entrada->params.scale, (int)entrada->params.zero_point);

    // 5. classifica 1 desenho de cada categoria
    int acertos = 0;
    for (int c = 0; c < NUM_CLASSES; c++) {
        const uint8_t* img = amostras_img + (c * AMOSTRAS_POR_CLASSE) * IMG_PIXELS;
        int verdadeiro = amostras_rotulo[c * AMOSTRAS_POR_CLASSE];

        // quantiza a entrada: pixel (0..255) -> float (0..1) -> int8
        for (int i = 0; i < IMG_PIXELS; i++) {
            float x = img[i] / 255.0f;
            int q = (int)lroundf(x / entrada->params.scale) + entrada->params.zero_point;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            entrada->data.int8[i] = (int8_t)q;
        }

        if (interpretador.Invoke() != kTfLiteOk) {
            printf("ERRO: Invoke falhou\n");
            return;
        }

        // acha a categoria com maior valor e converte para probabilidade
        int melhor = 0;
        for (int k = 1; k < NUM_CLASSES; k++) {
            if (saida->data.int8[k] > saida->data.int8[melhor]) melhor = k;
        }
        float prob = (saida->data.int8[melhor] - saida->params.zero_point) * saida->params.scale;

        bool ok = (melhor == verdadeiro);
        acertos += ok;
        printf("  %-11s -> %-11s %5.1f%%  %s\n",
               kCategorias[verdadeiro], kCategorias[melhor], prob * 100, ok ? "OK" : "ERROU");
    }

    printf("Acertos: %d de %d\n", acertos, NUM_CLASSES);
    printf("RAM livre no fim: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
}