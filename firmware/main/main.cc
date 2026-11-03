#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

extern "C" void app_main(void)
{
    printf("\n=== Pictionary ESP32: hello world ===\n");

    printf("TFLite Micro: schema versao %d\n", TFLITE_SCHEMA_VERSION);

    // informações do chip
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("Chip: ESP32 com %d nucleo(s), revisao %d\n", chip.cores, chip.revision);

    // tamanho da flash
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    printf("Flash: %lu MB\n", (unsigned long)(flash_bytes / (1024 * 1024)));

    // RAM livre (onde o modelo e a tensor arena vao ter que caber)
    printf("RAM livre: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

    // pisca uma mensagem a cada segundo, para mostrar que a placa esta viva
    int contador = 0;
    while (1) {
        printf("rodando... %d\n", contador++);
        vTaskDelay(pdMS_TO_TICKS(1000));   // espera 1000 ms
    }
}