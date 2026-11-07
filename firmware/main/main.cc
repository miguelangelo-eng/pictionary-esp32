#include <stdio.h>
#include <math.h>
#include <limits.h>

#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_credenciais.h"

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

#define WIFI_SSID  "Pictionary-IA"
#define WIFI_SENHA "tinyml123"      // mínimo de 8 caracteres
#define RODAR_BENCHMARK 0           // 1 = roda o benchmark no boot (leva ~2 minutos)

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

#if RODAR_BENCHMARK
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
#endif

// ---------------------------------------------------------------- Wi-Fi (modo Station)
// o Wi-Fi avisa o que acontece por meio de "eventos"; esta função reage a cada um
static void ao_evento_wifi(void* arg, esp_event_base_t base, int32_t id, void* dados)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();                                  // o Wi-Fi ligou: tenta conectar
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("Wi-Fi desconectado, tentando de novo...\n");
        esp_wifi_connect();                                  // caiu: tenta de novo
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evento = (ip_event_got_ip_t*)dados;
        printf("Conectado em \"%s\"!\n", WIFI_CASA_SSID);
        printf("No navegador do PC, abra: http://" IPSTR "\n", IP2STR(&evento->ip_info.ip));
    }
}

static void iniciar_wifi_casa()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // registra a função que reage aos eventos de Wi-Fi e de IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &ao_evento_wifi, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ao_evento_wifi, nullptr, nullptr));

    wifi_config_t config = {};
    strncpy((char*)config.sta.ssid, WIFI_CASA_SSID, sizeof(config.sta.ssid));
    strncpy((char*)config.sta.password, WIFI_CASA_SENHA, sizeof(config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("Conectando em \"%s\"...\n", WIFI_CASA_SSID);
}

// ---------------------------------------------------------------- servidor web
static esp_err_t pagina_inicial(httpd_req_t* req)
{
    const char* html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Pictionary ESP32</title></head>"
        "<body style='font-family:sans-serif;padding:16px'>"
        "<h1>Pictionary ESP32</h1>"
        "<p>A placa esta funcionando!</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void iniciar_servidor()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;   // a inferência vai rodar dentro do servidor (passo 7.3)

    httpd_handle_t servidor = nullptr;
    ESP_ERROR_CHECK(httpd_start(&servidor, &config));

    httpd_uri_t rota_inicial = {};
    rota_inicial.uri = "/";
    rota_inicial.method = HTTP_GET;
    rota_inicial.handler = pagina_inicial;
    httpd_register_uri_handler(servidor, &rota_inicial);

    printf("Servidor web iniciado\n");
}

// ---------------------------------------------------------------- main
extern "C" void app_main(void)
{
    printf("\n=== Pictionary ESP32 ===\n");

    // o Wi-Fi guarda dados de calibração do rádio na NVS (uma área da flash)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddConv2D();
    resolver.AddMaxPool2D();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    if (!iniciar(g_int8, resolver) || !iniciar(g_float32, resolver)) return;
    printf("RAM livre com os dois modelos: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

#if RODAR_BENCHMARK
    benchmark(g_int8);
    benchmark(g_float32);
    imprimir_tabela();
#endif

    iniciar_wifi_casa();
    iniciar_servidor();
    printf("RAM livre com tudo rodando: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
}