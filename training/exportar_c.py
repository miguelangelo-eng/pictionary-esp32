"""Converte os modelos .tflite em arrays C para o firmware da ESP32.

Uso (de qualquer pasta):
    python training/exportar_c.py
"""
import os

# caminhos calculados a partir da pasta deste script
PASTA_SCRIPT = os.path.dirname(os.path.abspath(__file__))
PASTA_MODELOS = os.path.join(PASTA_SCRIPT, "output")
PASTA_FIRMWARE = os.path.join(PASTA_SCRIPT, "..", "firmware", "main")

BYTES_POR_LINHA = 12


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


if __name__ == "__main__":
    os.makedirs(PASTA_FIRMWARE, exist_ok=True)
    gerar_array_c(os.path.join(PASTA_MODELOS, "modelo_int8.tflite"), "modelo_int8")
    gerar_array_c(os.path.join(PASTA_MODELOS, "modelo_float32.tflite"), "modelo_float32")