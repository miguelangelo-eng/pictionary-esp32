"""Converte os modelos .tflite em arrays C para o firmware da ESP32.

Uso (de qualquer pasta):
    python training/exportar_c.py
"""
import os
import numpy as np

# caminhos calculados a partir da pasta deste script
PASTA_SCRIPT = os.path.dirname(os.path.abspath(__file__))
PASTA_MODELOS = os.path.join(PASTA_SCRIPT, "output")
PASTA_FIRMWARE = os.path.join(PASTA_SCRIPT, "..", "firmware", "main")
CAMINHO_DATASET = os.path.join(PASTA_SCRIPT, "data", "dataset.npz")

BYTES_POR_LINHA = 12
AMOSTRAS_POR_CLASSE = 20
CATEGORIAS = [
    "apple", "bicycle", "car", "cat", "clock",
    "eye", "fish", "house", "key", "light bulb",
    "rainbow", "star", "sun", "tree", "umbrella",
]

def gerar_array_c(caminho_tflite, nome):
    # 1. lê o arquivo .tflite inteiro como bytes
    with open(caminho_tflite, "rb") as f:
        dados = f.read()

    # 2. monta as linhas do array: cada byte vira o texto "0x1c"
    linhas = []
    for i in range(0, len(dados), BYTES_POR_LINHA):
        pedaco = dados[i:i + BYTES_POR_LINHA]
        linhas.append("  " + ", ".join(f"0x{b:02x}" for b in pedaco))

    # 3. arquivo .h: só a DECLARAÇÃO (avisa que o array existe)
    with open(os.path.join(PASTA_FIRMWARE, f"{nome}.h"), "w", encoding="utf-8") as f:
        f.write("// Gerado por training/exportar_c.py -- nao edite a mao.\n")
        f.write("#pragma once\n\n")
        f.write(f"extern const unsigned char {nome}[];\n")
        f.write(f"extern const unsigned int {nome}_len;\n")

    # 4. arquivo .cc: a DEFINIÇÃO (os bytes de verdade)
    with open(os.path.join(PASTA_FIRMWARE, f"{nome}.cc"), "w", encoding="utf-8") as f:
        f.write("// Gerado por training/exportar_c.py -- nao edite a mao.\n")
        f.write(f'#include "{nome}.h"\n\n')
        f.write(f"alignas(16) const unsigned char {nome}[] = {{\n")
        f.write(",\n".join(linhas))
        f.write("\n};\n")
        f.write(f"const unsigned int {nome}_len = {len(dados)};\n")

    print(f"{nome}: {len(dados)} bytes ({len(dados) / 1024:.1f} KB) -> {nome}.h / {nome}.cc")

def exportar_amostras():
    d = np.load(CAMINHO_DATASET)
    X, y = d["X_teste"], d["y_teste"]   # X em uint8, shape (N, 784)

    # os primeiros 20 desenhos de cada categoria, agrupados por categoria
    indices = np.concatenate(
        [np.where(y == c)[0][:AMOSTRAS_POR_CLASSE] for c in range(len(CATEGORIAS))]
    )
    imagens = X[indices].astype(np.uint8)
    rotulos = y[indices].astype(np.uint8)
    n = len(rotulos)

    nomes = ", ".join(f'"{c}"' for c in CATEGORIAS)
    with open(os.path.join(PASTA_FIRMWARE, "amostras.h"), "w", encoding="utf-8") as f:
        f.write("// Gerado por training/exportar_c.py -- nao edite a mao.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define NUM_CLASSES {len(CATEGORIAS)}\n")
        f.write(f"#define AMOSTRAS_POR_CLASSE {AMOSTRAS_POR_CLASSE}\n")
        f.write(f"#define NUM_AMOSTRAS {n}\n")
        f.write("#define IMG_PIXELS 784\n\n")
        f.write(f"static const char* const kCategorias[NUM_CLASSES] = {{{nomes}}};\n\n")
        f.write("// desenhos 28x28 em uint8 (0..255), um apos o outro\n")
        f.write("extern const uint8_t amostras_img[];\n")
        f.write("extern const uint8_t amostras_rotulo[];\n")

    linhas = []
    plano = imagens.reshape(-1)
    for i in range(0, len(plano), 28):   # uma linha do desenho por linha de código
        linhas.append("  " + ", ".join(str(v) for v in plano[i:i + 28]))

    with open(os.path.join(PASTA_FIRMWARE, "amostras.cc"), "w", encoding="utf-8") as f:
        f.write("// Gerado por training/exportar_c.py -- nao edite a mao.\n")
        f.write('#include "amostras.h"\n\n')
        f.write("const uint8_t amostras_img[] = {\n")
        f.write(",\n".join(linhas))
        f.write("\n};\n\n")
        f.write("const uint8_t amostras_rotulo[] = {" + ", ".join(str(v) for v in rotulos) + "};\n")

    print(f"amostras: {n} desenhos ({n * 784 / 1024:.1f} KB) -> amostras.h / amostras.cc")

if __name__ == "__main__":
    os.makedirs(PASTA_FIRMWARE, exist_ok=True)
    gerar_array_c(os.path.join(PASTA_MODELOS, "modelo_int8.tflite"), "modelo_int8")
    gerar_array_c(os.path.join(PASTA_MODELOS, "modelo_float32.tflite"), "modelo_float32")
    exportar_amostras()