// Interface na tela touch da ESP32-2432S028 ("Cheap Yellow Display", 2,8" 320x240,
// toque resistivo XPT2046). Você desenha com o dedo, solta, e meio segundo depois a
// placa mostra o palpite do float32 e do int8, igual à página web.
//
// Inicialização da tela e do toque igual à do agent-city-display (mesma placa).
#include "tela.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PICTIONARY_LCD_ILI9341
#include "esp_lcd_ili9341.h"
#endif

#include "amostras.h"
#include "rasterizar.h"

static const char* TAG = "tela";

// opções booleanas do Kconfig ficam indefinidas quando desligadas
#ifndef CONFIG_PICTIONARY_LCD_MIRROR_X
#define CONFIG_PICTIONARY_LCD_MIRROR_X 0
#endif
#ifndef CONFIG_PICTIONARY_LCD_MIRROR_Y
#define CONFIG_PICTIONARY_LCD_MIRROR_Y 0
#endif

// ---------------------------------------------------------------- placa
namespace pino {
constexpr int LCD_SCLK = 14, LCD_MOSI = 13, LCD_MISO = 12, LCD_CS = 15, LCD_DC = 2, LCD_BL = 21;
constexpr int TP_SCLK = 25, TP_MOSI = 32, TP_MISO = 39, TP_CS = 33, TP_IRQ = 36;
}

constexpr int LCD_W = 240, LCD_H = 320;  // retrato nativo do painel; a interface usa o retrato
constexpr int TELA_W = LCD_W, TELA_H = LCD_H;

// Calibração do toque resistivo: o driver devolve o valor bruto do ADC (0..4095).
// Mesmos limites medidos na placa do agent-city-display.
namespace cal {
constexpr int RAW_X_MIN = 480, RAW_X_MAX = 3776;  // eixo que vira Y na tela
constexpr int RAW_Y_MIN = 295, RAW_Y_MAX = 3725;  // eixo que vira X na tela
}

static int escalar(int v, int lo, int hi, int out) {
    int r = (v - lo) * out / (hi - lo);
    return r < 0 ? 0 : r >= out ? out - 1 : r;
}

static void mapear_toque(esp_lcd_touch_handle_t, uint16_t* x, uint16_t* y, uint16_t*, uint8_t* n, uint8_t) {
    for (int i = 0; i < *n; i++) {
        // primeiro em paisagem (como no agent-city-display), depois gira 90 graus para o retrato
        int lx = escalar(y[i], cal::RAW_Y_MIN, cal::RAW_Y_MAX, LCD_H);
        int ly = escalar(x[i], cal::RAW_X_MIN, cal::RAW_X_MAX, LCD_W);
        int sx = LCD_W - 1 - ly, sy = lx;
#if CONFIG_PICTIONARY_TOUCH_MIRROR_X
        sx = TELA_W - 1 - sx;
#endif
#if CONFIG_PICTIONARY_TOUCH_MIRROR_Y
        sy = TELA_H - 1 - sy;
#endif
        x[i] = sx;
        y[i] = sy;
    }
}

static void luz_de_fundo(int porcento) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 5000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {};
    c.gpio_num = pino::LCD_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    c.duty = (1023 * porcento) / 100;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void iniciar_placa() {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = pino::LCD_SCLK;
    bus.mosi_io_num = pino::LCD_MOSI;
    bus.miso_io_num = pino::LCD_MISO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = TELA_W * 10 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = pino::LCD_CS;
    io_cfg.dc_gpio_num = pino::LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t painel = nullptr;
    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = -1;
    pcfg.bits_per_pixel = 16;
#if CONFIG_PICTIONARY_LCD_ILI9341
    pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &pcfg, &painel));
#else
    pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &painel));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_reset(painel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(painel));
#if CONFIG_PICTIONARY_LCD_INVERT
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(painel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(painel, true));

    // toque: barramento SPI próprio
    spi_bus_config_t tbus = {};
    tbus.sclk_io_num = pino::TP_SCLK;
    tbus.mosi_io_num = pino::TP_MOSI;
    tbus.miso_io_num = pino::TP_MISO;
    tbus.quadwp_io_num = -1;
    tbus.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &tbus, SPI_DMA_DISABLED));
    esp_lcd_panel_io_handle_t tio = nullptr;
    esp_lcd_panel_io_spi_config_t tio_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(pino::TP_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &tio_cfg, &tio));
    esp_lcd_touch_config_t tcfg = {};
    tcfg.x_max = 4096;  // valor bruto; mapear_toque converte
    tcfg.y_max = 4096;
    tcfg.rst_gpio_num = GPIO_NUM_NC;
    // Sem o pino de interrupção: com ele, o esp_lvgl_port só lê o toque no instante em que o
    // dedo encosta (modo evento), e o arrasto nunca é lido. Lendo a cada 10 ms dá para desenhar.
    tcfg.int_gpio_num = GPIO_NUM_NC;
    tcfg.process_coordinates = mapear_toque;
    esp_lcd_touch_handle_t toque = nullptr;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tio, &tcfg, &toque));

    // LVGL
    lvgl_port_cfg_t lcfg = ESP_LVGL_PORT_INIT_CONFIG();
    lcfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&lcfg));

    lvgl_port_display_cfg_t dcfg = {};
    dcfg.io_handle = io;
    dcfg.panel_handle = painel;
    dcfg.buffer_size = TELA_W * 10;  // 10 linhas, dois buffers: ~13 KB de RAM com DMA
    dcfg.double_buffer = true;
    dcfg.hres = TELA_W;
    dcfg.vres = TELA_H;
    dcfg.rotation.swap_xy = false;  // retrato nativo do painel
    dcfg.rotation.mirror_x = CONFIG_PICTIONARY_LCD_MIRROR_X;
    dcfg.rotation.mirror_y = CONFIG_PICTIONARY_LCD_MIRROR_Y;
    dcfg.color_format = LV_COLOR_FORMAT_RGB565;
    dcfg.flags.buff_dma = true;
    dcfg.flags.swap_bytes = true;
    lv_display_t* disp = lvgl_port_add_disp(&dcfg);

    lvgl_port_touch_cfg_t tc = {};
    tc.disp = disp;
    tc.handle = toque;
    lv_indev_t* indev = lvgl_port_add_touch(&tc);
    // lê o toque a cada 10 ms (o padrão é ~33 ms): traços mais lisos
    lvgl_port_lock(0);
    if (indev && lv_indev_get_read_timer(indev)) lv_timer_set_period(lv_indev_get_read_timer(indev), 10);
    lvgl_port_unlock();

    luz_de_fundo(80);
}

// ---------------------------------------------------------------- desenho
// As fontes do LVGL só têm ASCII, então os nomes vão sem acento.
static const char* const kNomes[] = {"maca",   "bicicleta", "carro", "gato",     "relogio",
                                     "olho",   "peixe",     "casa",  "chave",    "lampada",
                                     "arco-iris", "estrela", "sol",  "arvore",   "guarda-chuva"};
static_assert(sizeof(kNomes) / sizeof(kNomes[0]) == NUM_CLASSES, "um nome por categoria");

constexpr int QUADRO = 228;          // área de desenho quadrada, em pixels
constexpr int MAX_PONTOS = 1000;
constexpr int MAX_TRACOS = 64;
constexpr int PINCEL = 6;            // espessura do traço na tela

static Ponto s_pontos[MAX_PONTOS];
static uint16_t s_inicio[MAX_TRACOS];
static int s_n_pontos = 0, s_n_tracos = 0;
static bool s_desenhando = false;

static lv_obj_t* s_quadro;
static lv_obj_t* s_palpite[2];
static lv_obj_t* s_info[2];
static lv_obj_t* s_previa;
static lv_obj_t* s_status;
static lv_timer_t* s_timer;

constexpr int PREVIA = 56;           // 28x28 ampliado 2x
LV_DRAW_BUF_DEFINE_STATIC(s_previa_buf, PREVIA, PREVIA, LV_COLOR_FORMAT_L8);

static TelaPrever s_prever;
static TaskHandle_t s_tarefa;
static uint8_t s_pixels[IMG_PIXELS];  // último desenho 28x28 (escrito na task do LVGL)

static void mostrar_previa(const uint8_t* px) {
    lv_draw_buf_t* b = &s_previa_buf;
    for (int y = 0; y < PREVIA; y++) {
        uint8_t* linha = b->data + y * b->header.stride;
        for (int x = 0; x < PREVIA; x++) linha[x] = px ? px[(y / 2) * 28 + x / 2] : 0;
    }
    lv_obj_invalidate(s_previa);
}

static void invalidar_segmento(Ponto a, Ponto b) {
    lv_area_t q;
    lv_obj_get_coords(s_quadro, &q);
    lv_area_t r;
    r.x1 = q.x1 + LV_MIN(a.x, b.x) - PINCEL;
    r.y1 = q.y1 + LV_MIN(a.y, b.y) - PINCEL;
    r.x2 = q.x1 + LV_MAX(a.x, b.x) + PINCEL;
    r.y2 = q.y1 + LV_MAX(a.y, b.y) + PINCEL;
    lv_obj_invalidate_area(s_quadro, &r);
}

static Ponto ponto_no_quadro() {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t q;
    lv_obj_get_coords(s_quadro, &q);
    Ponto r;
    r.x = (int16_t)LV_CLAMP(0, p.x - q.x1, QUADRO - 1);
    r.y = (int16_t)LV_CLAMP(0, p.y - q.y1, QUADRO - 1);
    return r;
}

static void ao_tocar_quadro(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_timer_pause(s_timer);
        if (s_n_tracos >= MAX_TRACOS || s_n_pontos >= MAX_PONTOS) return;  // cheio
        Ponto p = ponto_no_quadro();
        s_inicio[s_n_tracos++] = (uint16_t)s_n_pontos;
        s_pontos[s_n_pontos++] = p;
        s_desenhando = true;
        invalidar_segmento(p, p);
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_desenhando || s_n_pontos >= MAX_PONTOS) return;
        Ponto p = ponto_no_quadro();
        Ponto ant = s_pontos[s_n_pontos - 1];
        if (LV_ABS(p.x - ant.x) + LV_ABS(p.y - ant.y) < 2) return;  // ignora o tremido do toque
        s_pontos[s_n_pontos++] = p;
        invalidar_segmento(ant, p);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_desenhando) return;
        s_desenhando = false;
        lv_timer_reset(s_timer);
        lv_timer_resume(s_timer);
    }
}

// desenha os traços guardados por cima do fundo branco do quadro
static void ao_desenhar_quadro(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t q;
    lv_obj_get_coords(s_quadro, &q);
    const lv_area_t& clip = layer->_clip_area;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(0x111111);
    d.width = PINCEL;
    d.round_start = 1;
    d.round_end = 1;

    for (int k = 0; k < s_n_tracos; k++) {
        int ini = s_inicio[k];
        int fim = (k + 1 < s_n_tracos) ? s_inicio[k + 1] : s_n_pontos;
        for (int i = ini; i < fim; i++) {
            Ponto a = s_pontos[i];
            Ponto b = (i + 1 < fim) ? s_pontos[i + 1] : a;
            if (i + 1 >= fim && fim - ini > 1) break;  // traço com 2+ pontos: só os segmentos
            // pula segmentos fora da faixa que está sendo redesenhada
            if (q.x1 + LV_MAX(a.x, b.x) + PINCEL < clip.x1 || q.x1 + LV_MIN(a.x, b.x) - PINCEL > clip.x2 ||
                q.y1 + LV_MAX(a.y, b.y) + PINCEL < clip.y1 || q.y1 + LV_MIN(a.y, b.y) - PINCEL > clip.y2)
                continue;
            d.p1.x = q.x1 + a.x;
            d.p1.y = q.y1 + a.y;
            d.p2.x = q.x1 + b.x + (a.x == b.x && a.y == b.y ? 1 : 0);  // um toque só vira um ponto
            d.p2.y = q.y1 + b.y;
            lv_draw_line(layer, &d);
        }
    }
}

static void limpar_resultados() {
    for (int m = 0; m < 2; m++) {
        lv_label_set_text(s_palpite[m], "-");
        lv_label_set_text(s_info[m], "");
    }
}

static void ao_limpar(lv_event_t*) {
    lv_timer_pause(s_timer);
    s_n_pontos = s_n_tracos = 0;
    s_desenhando = false;
    lv_obj_invalidate(s_quadro);
    mostrar_previa(nullptr);
    limpar_resultados();
}

// meio segundo depois de soltar o dedo: prepara o 28x28 e acorda a task do palpite
static void ao_esperar(lv_timer_t*) {
    lv_timer_pause(s_timer);
    if (s_n_tracos == 0) return;
    if (!rasterizar(s_pontos, s_n_pontos, s_inicio, s_n_tracos, s_pixels)) {
        ESP_LOGE(TAG, "sem RAM para rasterizar");
        return;
    }
    mostrar_previa(s_pixels);
    for (int m = 0; m < 2; m++) lv_label_set_text(s_info[m], "pensando...");
    xTaskNotifyGive(s_tarefa);
}

// task própria: a inferência leva ~0,5 s e não pode travar a tela
static void tarefa_palpite(void*) {
    static uint8_t px[IMG_PIXELS];
    static const char* const kModelo[2] = {"float32", "int8"};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lvgl_port_lock(0);
        memcpy(px, s_pixels, sizeof(px));
        lvgl_port_unlock();

        TelaPalpite res[2] = {};
        s_prever(px, res);

        lvgl_port_lock(0);
        for (int m = 0; m < 2; m++) {
            lv_label_set_text(s_palpite[m], kNomes[res[m].classe]);
            lv_label_set_text_fmt(s_info[m], "%d%%  %d ms", (int)(res[m].prob * 100 + 0.5f), (int)(res[m].ms + 0.5f));
            printf("  [tela %-7s] %s %.0f%%\n", kModelo[m], kNomes[res[m].classe], res[m].prob * 100);
        }
        lvgl_port_unlock();
    }
}

// ---------------------------------------------------------------- interface
static lv_obj_t* rotulo(lv_obj_t* pai, int x, int y, int w, const lv_font_t* fonte, uint32_t cor, const char* texto) {
    lv_obj_t* l = lv_label_create(pai);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(l, fonte, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(cor), 0);
    lv_label_set_text(l, texto);
    return l;
}

static void montar_interface() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_quadro = lv_obj_create(scr);
    lv_obj_set_pos(s_quadro, (TELA_W - QUADRO) / 2, 4);
    lv_obj_set_size(s_quadro, QUADRO, QUADRO);
    lv_obj_set_style_bg_color(s_quadro, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_quadro, 0, 0);
    lv_obj_set_style_radius(s_quadro, 0, 0);
    lv_obj_set_style_pad_all(s_quadro, 0, 0);
    lv_obj_remove_flag(s_quadro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_quadro, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_quadro, ao_tocar_quadro, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(s_quadro, ao_desenhar_quadro, LV_EVENT_DRAW_MAIN, nullptr);

    // embaixo do quadro: float32 | int8 | prévia 28x28
    const int y0 = 4 + QUADRO + 4;  // 236
    const int col = 84;
    rotulo(scr, 6, y0, col, &lv_font_montserrat_12, 0x999999, "FLOAT32");
    s_palpite[0] = rotulo(scr, 6, y0 + 14, col, &lv_font_montserrat_16, 0x4cc38a, "-");
    s_info[0] = rotulo(scr, 6, y0 + 34, col, &lv_font_montserrat_12, 0xbbbbbb, "");
    rotulo(scr, 6 + col + 4, y0, col, &lv_font_montserrat_12, 0x999999, "INT8");
    s_palpite[1] = rotulo(scr, 6 + col + 4, y0 + 14, col, &lv_font_montserrat_16, 0xf5a524, "-");
    s_info[1] = rotulo(scr, 6 + col + 4, y0 + 34, col, &lv_font_montserrat_12, 0xbbbbbb, "");

    LV_DRAW_BUF_INIT_STATIC(s_previa_buf);
    s_previa = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(s_previa, &s_previa_buf);
    lv_obj_set_pos(s_previa, TELA_W - PREVIA - 6, y0);
    lv_obj_set_style_outline_width(s_previa, 1, 0);
    lv_obj_set_style_outline_color(s_previa, lv_color_hex(0x444444), 0);
    mostrar_previa(nullptr);

    // última linha: status (IP) | Limpar
    const int yb = TELA_H - 26;
    s_status = rotulo(scr, 6, yb + 5, 130, &lv_font_montserrat_12, 0x999999, "Desenhe!");

    lv_obj_t* botao = lv_button_create(scr);
    lv_obj_set_pos(botao, 140, yb);
    lv_obj_set_size(botao, TELA_W - 140 - 6, 24);
    lv_obj_set_style_bg_color(botao, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(botao, ao_limpar, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* t = lv_label_create(botao);
    lv_label_set_text(t, "Limpar");
    lv_obj_set_style_text_color(t, lv_color_hex(0xeeeeee), 0);
    lv_obj_center(t);

    s_timer = lv_timer_create(ao_esperar, CONFIG_PICTIONARY_PALPITE_MS, nullptr);
    lv_timer_pause(s_timer);
}

void tela_iniciar(TelaPrever prever) {
    s_prever = prever;
    iniciar_placa();
    xTaskCreate(tarefa_palpite, "palpite", 8192, nullptr, 4, &s_tarefa);

    lvgl_port_lock(0);
    montar_interface();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "tela e toque prontos");
}

void tela_status(const char* texto) {
    if (!s_status) return;
    lvgl_port_lock(0);
    lv_label_set_text(s_status, texto);
    lvgl_port_unlock();
}
