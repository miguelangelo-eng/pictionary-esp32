# 🎨 Pictionary com IA na ESP32

**Uma rede neural que reconhece desenhos rodando dentro de um microcontrolador de 520 KB de RAM.**
Você desenha no navegador, e a própria ESP32 serve a página, roda o modelo e responde. Sem nuvem e sem internet.

O projeto também é um **benchmark float32 vs int8** medido na placa: a mesma rede, em duas versões, comparadas em tamanho, memória, velocidade e acurácia.


##  Destaques

| | float32 | int8 | Ganho do int8 |
|---|---|---|---|
| **Tamanho do modelo** | 391,2 KB | 108,2 KB | **3,6× menor** |
| **RAM (tensor arena)** | 63,7 KB | 18,9 KB | **3,4× menos** |
| **Tempo por desenho na ESP32** | 401,2 ms | 115,6 ms | **3,5× mais rápido** |
| **Acurácia (18 mil desenhos de teste)** | 95,32% | 95,27% | -0,05 ponto |

- As duas versões dão **a mesma resposta em 99,72%** dos desenhos de teste.
- CNN com **99.087 parâmetros**, treinada em **15 categorias** do dataset [Quick, Draw!](https://quickdraw.withgoogle.com/data) do Google.
- Tempos medidos na ESP32-WROOM-32 a 240 MHz, compilado com `-O2` e flash a 80 MHz.

---

##  Como funciona

```
 Navegador (PC ou celular)                          ESP32
 ─────────────────────────                          ─────
 desenho no canvas
       │
 pré-processamento (JavaScript)
 • traço branco em fundo preto
 • centraliza e amplia o desenho
 • reduz para 28×28 com bordas suaves
       │
 784 bytes ──────── POST /prever ─────────────►  TensorFlow Lite Micro
                                                 ├─ modelo float32 → Invoke()
                                                 └─ modelo int8    → Invoke()
       │                                                │
 mostra as duas previsões ◄──────── JSON ◄──────────────┘
 e o tempo de cada uma
```

A página é embutida no firmware e servida pela própria placa, que se conecta ao Wi-Fi da casa (modo Station).

Categorias: maçã, bicicleta, carro, gato, relógio, olho, peixe, casa, chave, lâmpada, arco-íris, estrela, sol, árvore e guarda-chuva.

---

##  Benchmark na placa

300 desenhos do conjunto de teste, gravados na flash junto com o firmware. O tempo medido é só o `Invoke()`, em média, depois de uma inferência de aquecimento.

### O mesmo modelo em três configurações

| Configuração | float32 | int8 | int8 mais rápido |
|---|---|---|---|
| ① Padrão do ESP-IDF (CPU 160 MHz, `-Og`, flash 40 MHz) | 831,8 ms | 182,4 ms | 4,6× |
| ② + CPU 240 MHz e `-O2` | 690,8 ms | 125,5 ms | 5,5× |
| ③ + flash a 80 MHz | **401,2 ms** | **115,6 ms** | **3,5×** |

###  O que o experimento da flash mostrou

Na configuração padrão, o int8 parecia **4,6×** mais rápido. Mas os pesos do modelo ficam na **flash**, e o float32 tem 4× mais bytes para ler a cada inferência.

**Hipótese:** o float32 estava limitado pela velocidade da memória (*memory-bound*), e não pelas contas.

**Teste:** dobrar a velocidade da flash (40 → 80 MHz).

| | Flash 40 MHz | Flash 80 MHz | Ganho |
|---|---|---|---|
| float32 | 690,8 ms | 401,2 ms | **1,72×** |
| int8 | 125,5 ms | 115,6 ms | 1,09× |

**Conclusão:** o float32 passava boa parte do tempo esperando os pesos chegarem da flash. A vantagem real do int8, com o hardware bem configurado, é **3,5×**. Um benchmark só faz sentido junto com as condições em que foi medido.

Outros detalhes:
- No PC, as duas versões tinham praticamente **o mesmo tempo**. Na ESP32, o int8 usa os kernels do **ESP-NN**, e o float32 cai na implementação de referência do TFLite Micro. **O ganho da quantização depende do hardware.**
- O tempo de inferência é extremamente estável: variação de ~0,03 ms entre o mínimo e o máximo em 300 desenhos.

---

##  O modelo

| Camada | Saída | Parâmetros |
|---|---|---|
| Conv2D 16 filtros 3×3 + MaxPool | 14×14×16 | 160 |
| Conv2D 32 filtros 3×3 + MaxPool | 7×7×32 | 4.640 |
| Conv2D 64 filtros 3×3 + MaxPool | 3×3×64 | 18.496 |
| Flatten + Dense 128 | 128 | 73.856 |
| Dense 15 (softmax) | 15 | 1.935 |
| **Total** | | **99.087** |

A camada `Dense(128)` concentra **75% dos parâmetros**: é o primeiro lugar a mexer para encolher o modelo.

**Treino**
- 12 mil desenhos por categoria, divididos em 80% treino, 10% validação e 10% teste.
- **Data augmentation** (rotação, translação e zoom leves) para lidar com desenhos tortos ou descentralizados, e **EarlyStopping** monitorando a perda de validação.
- Sem a augmentation, o modelo começava a decorar o treino a partir da 5ª época (overfitting). Com ela, as curvas de treino e validação andam juntas.

**Conversão**
- TensorFlow Lite **float32** e **int8 completo** (pesos, ativações, entrada e saída), calibrado com 500 desenhos do treino.
- Quantização da entrada: `scale = 1/255`, `zero_point = -128`, ou seja, `q = pixel − 128`. A entrada não perde informação.
- Saída int8 com `scale = 1/256`: por isso a maior "certeza" possível no int8 é **99,6%**.

### O que o modelo confunde


| Confusão | Vezes (de 1.200) | Por quê |
|---|---|---|
| lâmpada → chave | 27 | Em 28×28, as duas viram "um círculo com uma parte listrada" |
| sol → estrela | 22 | Os raios do sol parecem as pontas da estrela |
| guarda-chuva → árvore | 21 | Algo largo em cima de uma haste vertical |
| bicicleta → carro | 19 | Duas rodas embaixo |

A aposta inicial era que os círculos (relógio, sol, maçã) seriam confundidos entre si. Não foram: cada um tem um detalhe marcante (ponteiros, raios, cabinho). **A rede não pensa em conceitos, pensa em traços.**

Mais fácil: **estrela (98,5%)**. Mais difícil: **lâmpada (90,8%)**.

---

##  Estrutura do repositório

```
pictionary-esp32/
├── training/
│   ├── 01_dados.ipynb          baixa o Quick, Draw!, explora e monta treino/validação/teste
│   ├── 02_modelo.ipynb         treina a CNN e avalia (matriz de confusão)
│   ├── 03_conversao.ipynb      converte para TFLite float32 e int8 e compara
│   ├── exportar_c.py           gera os modelos e as amostras de teste como arrays C
│   └── output/                 modelo .keras e os dois .tflite
├── firmware/                   projeto ESP-IDF
│   ├── partitions.csv          partição de 3 MB para o app
│   ├── sdkconfig               240 MHz, -O2, flash 80 MHz, 4 MB
│   ├── sdkconfig.tela          liga a tela touch (opcional)
│   └── main/
│       ├── main.cc             TFLite Micro, benchmark, Wi-Fi e servidor HTTP
│       ├── index.html          página de desenho (embutida no firmware)
│       ├── tela.cc / .h        interface na tela touch ESP32-2432S028 (opcional)
│       ├── rasterizar.cc / .h  pré-processamento do index.html, em C (para a tela)
│       ├── Kconfig.projbuild   menu "Pictionary" do menuconfig
│       ├── modelo_*.cc / .h    modelos como arrays C (gerados)
│       ├── amostras.cc / .h    300 desenhos de teste para o benchmark (gerados)
│       └── wifi_credenciais.exemplo.h
└── resultados.md               resultados de todas as medições
```

---

##  Como reproduzir

### O que você precisa
- Uma **ESP32** com 4 MB de flash (testado na ESP32-WROOM-32).
- **Python 3.12+** com TensorFlow.
- **ESP-IDF v6.1** (ou v5.x) — a extensão ESP-IDF do VS Code é o caminho mais fácil.

### 1. Treinar e converter (opcional: os modelos já estão no repositório)

```bash
cd training
python -m venv .venv
.venv\Scripts\activate          # no Linux/Mac: source .venv/bin/activate
pip install tensorflow numpy matplotlib ipykernel
```

Rode os notebooks em ordem: `01_dados` → `02_modelo` → `03_conversao`. Depois gere os arquivos C do firmware:

```bash
python exportar_c.py
```

### 2. Configurar o Wi-Fi

Copie `firmware/main/wifi_credenciais.exemplo.h` para `firmware/main/wifi_credenciais.h` e preencha com a sua rede. A ESP32 só funciona em **2,4 GHz**.

```cpp
#define WIFI_CASA_SSID  "nome-da-sua-rede"
#define WIFI_CASA_SENHA "senha-da-sua-rede"
```

O `wifi_credenciais.h` está no `.gitignore` e nunca vai para o repositório.

### 3. Gravar na placa

Na pasta `firmware`:

```bash
idf.py set-target esp32
idf.py build flash monitor
```

O monitor mostra o endereço da placa:

```
No navegador do PC, abra: http://192.168.0.2
```

Abra esse endereço num navegador conectado à mesma rede e desenhe.

### 4. Rodar o benchmark

No `main.cc`, mude `#define RODAR_BENCHMARK 0` para `1`. No boot, a placa roda os 300 desenhos de teste nas duas versões (~2 minutos) e imprime a tabela no monitor.

---

##  Opcional: desenhar na tela touch da placa

Também dá para jogar sem navegador, desenhando direto na tela da **ESP32-2432S028** ("Cheap Yellow Display", 2,8" 320×240, toque resistivo XPT2046). É uma opção de compilação: desligada, o firmware é exatamente o projeto original.

```
 ┌──────────────────────────┐
 │                          │
 │     área de desenho      │
 │         228×228          │
 │                          │
 ├─────────┬─────────┬──────┤
 │ FLOAT32 │ INT8    │[28×28│
 │ gato    │ gato    │prévia│
 │ 87% 401ms 85% 116ms      │
 ├─────────┴───┬─────┴──────┤
 │ Web: IP     │  Limpar    │
 └─────────────┴────────────┘
```

- Tela na vertical (240×320). Desenhe com o dedo ou a canetinha. Ao soltar, depois de 0,6 s, a placa mostra o palpite dos dois modelos e o que o modelo "vê" (28×28).
- O pré-processamento é o mesmo do `index.html`, reescrito em C (`rasterizar.cc`).
- A página web continua funcionando ao mesmo tempo, e o IP aparece na tela. Sem o `wifi_credenciais.h`, a placa funciona só pela tela.

Na pasta `firmware`:

```bash
idf.py -B build_tela -D SDKCONFIG=build_tela/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig;sdkconfig.tela" build flash monitor
```

O `sdkconfig.tela` liga a opção por cima do `sdkconfig` original, que não é alterado: a configuração da tela fica em `build_tela/`. As dependências da tela (LVGL 9, `esp_lvgl_port`, drivers ST7789/ILI9341 e XPT2046) só são baixadas nesse modo.

Ajustes em `idf.py -B build_tela -D SDKCONFIG=build_tela/sdkconfig menuconfig` > **Pictionary**:

| Opção | Padrão | Quando mudar |
|---|---|---|
| Controlador da tela | ST7789 | Placas só com micro USB costumam usar ILI9341 |
| Inverter cores | desligado | Cores em negativo |
| Tela: espelhar X / Y | desligado | Imagem de cabeça para baixo ou espelhada |
| Toque: espelhar X / Y | desligado | O traço aparece longe do dedo |
| Espera para adivinhar | 600 ms | |

Pinagem: LCD SCLK/MOSI/MISO/CS/DC/BL = 14/13/12/15/2/21; toque CLK/MOSI/MISO/CS/IRQ = 25/32/39/33/36.

---

##  Detalhes técnicos

<details>
<summary><b>Pré-processamento no navegador</b></summary>

O modelo aprendeu com desenhos num formato específico, então o desenho do navegador precisa chegar igual:

1. Calcula a caixa em volta de todos os traços.
2. Amplia com uma escala única (mantém a proporção) até o lado maior ocupar ~26 dos 28 pixels.
3. Centraliza.
4. Redesenha os traços em **branco sobre preto** numa tela de 256×256.
5. Reduz em etapas (256 → 128 → 56 → 28) para obter bordas suaves, como no dataset.

</details>

<details>
<summary><b>Memória</b></summary>

- Os modelos ficam na **flash** como arrays `const` alinhados em 16 bytes: não ocupam RAM.
- Na RAM ficam só as tensor arenas (~83 KB para as duas versões) e o Wi-Fi + servidor HTTP (~87 KB no total).
- Com tudo rodando, sobram ~90 KB livres.
- A tabela de partições padrão reserva só 1 MB para o app. Com Wi-Fi, modelos e amostras, o firmware tem ~1,3 MB, por isso o `partitions.csv` reserva 3 MB.

</details>

<details>
<summary><b>Operações do TFLite Micro</b></summary>

As duas versões usam as mesmas 5 operações, registradas manualmente com um `MicroMutableOpResolver<5>` para economizar flash: `CONV_2D`, `MAX_POOL_2D`, `RESHAPE`, `FULLY_CONNECTED` e `SOFTMAX`.

</details>

---

## 📜 Créditos e licença

- Dataset: [Quick, Draw!](https://github.com/googlecreativelab/quickdraw-dataset), do Google, sob a licença [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- [TensorFlow Lite Micro](https://github.com/espressif/esp-tflite-micro) e [ESP-NN](https://github.com/espressif/esp-nn), da Espressif.
- Código deste repositório: <!-- TODO: escolha uma licença, por exemplo MIT --> licença MIT.

Feito por [@miguelangelo-eng](https://github.com/miguelangelo-eng). <!-- TODO: adicione o link do seu LinkedIn -->
