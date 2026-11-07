# Configuração padrão (160 MHz, -Og)

Avaliação com 300 desenhos de teste.

| Métrica                | float32   | int8      | Ganho do int8   |
|------------------------|----------:|----------:|-----------------|
| Tamanho do modelo      | 391.2 KB  | 108.2 KB  | 3.6x menor      |
| RAM (tensor arena)     | 63.7 KB   | 18.9 KB   | 3.4x menos RAM  |
| Tempo médio            | 831.78 ms | 182.41 ms | 4.6x mais rápido |
| Acurácia               | 96.0 %    | 96.0 %    | igual           |
