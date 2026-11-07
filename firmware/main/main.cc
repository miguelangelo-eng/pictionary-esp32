#include <stdio.h>
#include <math.h>
#include <limits.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

#include "amostras.h"
#include "modelo_float32.h"
#include "modelo_int8.h"

// tudo o que precisamos guardar de cada versão do modelo
struct Modelo {
    const char* nome;
    const unsigned char* dados;
    unsigned int tamanho;
    size_t arena_tamanho;
    tflite::MicroInterpreter* interpretador;
    // resultados do benchmark
    float acuracia;
    int64_t media_us, min_us, max_us;
};

static Modelo g_int8    = {"int8",    modelo_int8,    modelo_int8_len,    24 * 1024, nullptr, 0, 0, 0, 0};
static Modelo g_float32 = {"float32", modelo_float32, modelo_float32_len, 96 * 1024, nullptr, 0, 0, 0, 0};

// ---------------------------------------------------------------- preparação
static bool iniciar(Modelo& m, const tflite::MicroOpResolver& resolver)
{
    const tflite::Model* modelo = tflite::GetModel(m.dados);
    if (modelo->version() != TFLITE_SCHEMA_VERSION) {
        printf("[%s] ERRO: schema incompativel\n", m.nome);
        return false;
    }
    uint8_t* arena = (uint8_t*)heap_caps_malloc(m.arena_tamanho, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (arena == nullptr) {
        printf("[%s] ERRO: sem RAM para a arena\n", m.nome);
        return false;
    }
    m.interpretador = new tflite::MicroInterpreter(modelo, resolver, arena, m.arena_tamanho);
    if (m.interpretador->AllocateTensors() != kTfLiteOk) {
        printf("[%s] ERRO: AllocateTensors falhou (aumente a arena)\n", m.nome);
        return false;
    }
    printf("[%s] modelo %u bytes | arena usou %u de %u bytes\n", m.nome, m.tamanho,
           (unsigned)m.interpretador->arena_used_bytes(), (unsigned)m.arena_tamanho);
    return true;
}

// coloca um desenho (uint8 0..255) na entrada, no formato que cada versão espera
static void preencher_entrada(TfLiteTensor* t, const uint8_t* img)
{
    if (t->type == kTfLiteFloat32) {
        for (int i = 0; i < IMG_PIXELS; i++) t->data.f[i] = img[i] / 255.0f;
    } else {
        for (int i = 0; i < IMG_PIXELS; i++) {
            int q = (int)lroundf((img[i] / 255.0f) / t->params.scale) + t->params.zero_point;
            t->data.int8[i] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
        }
    }
}

// categoria com o maior valor na saída
static int classe_prevista(const TfLiteTensor* t)
{
    int melhor = 0;
    for (int k = 1; k < NUM_CLASSES; k++) {
        bool maior = (t->type == kTfLiteFloat32) ? (t->data.f[k] > t->data.f[melhor])
                                                 : (t->data.int8[k] > t->data.int8[melhor]);
        if (maior) melhor = k;
    }
    return melhor;
}

// ---------------------------------------------------------------- benchmark
static void benchmark(Modelo& m)
{
    TfLiteTensor* entrada = m.interpretador->input(0);
    const TfLiteTensor* saida = m.interpretador->output(0);

    // aquecimento: a primeira inferência é mais lenta (cache da flash vazio)
    preencher_entrada(entrada, amostras_img);
    m.interpretador->Invoke();

    int acertos = 0;
    int64_t total = 0, minimo = INT64_MAX, maximo = 0;

    for (int n = 0; n < NUM_AMOSTRAS; n++) {
        preencher_entrada(entrada, amostras_img + n * IMG_PIXELS);

        int64_t t0 = esp_timer_get_time();          // microssegundos desde o boot
        m.interpretador->Invoke();
        int64_t dt = esp_timer_get_time() - t0;     // só o tempo da inferência

        total += dt;
        if (dt < minimo) minimo = dt;
        if (dt > maximo) maximo = dt;
        if (classe_prevista(saida) == amostras_rotulo[n]) acertos++;

        vTaskDelay(1);  // depois de CADA inferência: o watchdog nunca espera mais que 1 inferência
        if (n % 50 == 49) printf("  [%s] %d/%d\n", m.nome, n + 1, NUM_AMOSTRAS);  // progresso
        }

    m.acuracia = (float)acertos / NUM_AMOSTRAS;
    m.media_us = total / NUM_AMOSTRAS;
    m.min_us = minimo;
    m.max_us = maximo;
    printf("[%s] acuracia %.1f%% | media %.2f ms | min %.2f ms | max %.2f ms\n", m.nome,
           m.acuracia * 100, m.media_us / 1000.0, m.min_us / 1000.0, m.max_us / 1000.0);
}

static void imprimir_tabela()
{
    const Modelo& f = g_float32;
    const Modelo& q = g_int8;
    printf("\n");
    printf("+------------------------+-------------+-------------+\n");
    printf("| %3d desenhos de teste  |   float32   |    int8     |\n", NUM_AMOSTRAS);
    printf("+------------------------+-------------+-------------+\n");
    printf("| Tamanho do modelo      | %8.1f KB | %8.1f KB |\n", f.tamanho / 1024.0, q.tamanho / 1024.0);
    printf("| RAM (tensor arena)     | %8.1f KB | %8.1f KB |\n",
           f.interpretador->arena_used_bytes() / 1024.0, q.interpretador->arena_used_bytes() / 1024.0);
    printf("| Tempo medio            | %8.2f ms | %8.2f ms |\n", f.media_us / 1000.0, q.media_us / 1000.0);
    printf("| Acuracia               | %9.1f %% | %9.1f %% |\n", f.acuracia * 100, q.acuracia * 100);
    printf("+------------------------+-------------+-------------+\n");
    printf("  int8: %.1fx menor, %.1fx menos RAM, %.1fx mais rapido\n",
           (float)f.tamanho / q.tamanho,
           (float)f.interpretador->arena_used_bytes() / q.interpretador->arena_used_bytes(),
           (float)f.media_us / q.media_us);
    #if defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
    const char* otimizacao = "-O2 (performance)";
    #elif defined(CONFIG_COMPILER_OPTIMIZATION_SIZE)
        const char* otimizacao = "-Os (tamanho)";
    #elif defined(CONFIG_COMPILER_OPTIMIZATION_NONE)
        const char* otimizacao = "-O0 (nenhuma)";
    #else
        const char* otimizacao = "-Og (debug)";
    #endif
        printf("  CPU a %d MHz | compilado com %s\n\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, otimizacao);
}

// ---------------------------------------------------------------- main
extern "C" void app_main(void)
{
    printf("\n=== Pictionary ESP32: benchmark float32 vs int8 ===\n");
    printf("RAM livre no inicio: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

    // as duas versões usam as mesmas 5 operações, então um resolver serve para as duas
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddConv2D();
    resolver.AddMaxPool2D();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    if (!iniciar(g_int8, resolver) || !iniciar(g_float32, resolver)) return;
    printf("RAM livre com os dois modelos: %lu bytes\n\n", (unsigned long)esp_get_free_heap_size());

    printf("rodando %d desenhos em cada versao...\n", NUM_AMOSTRAS);
    benchmark(g_int8);
    benchmark(g_float32);
    imprimir_tabela();
}