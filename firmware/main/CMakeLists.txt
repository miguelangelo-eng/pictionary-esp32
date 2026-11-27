set(fontes "main.cc" "modelo_int8.cc" "modelo_float32.cc" "amostras.cc")

# tela touch (opcional): idf.py menuconfig > Pictionary, ou o arquivo sdkconfig.tela
if(CONFIG_PICTIONARY_TELA)
    list(APPEND fontes "tela.cc" "rasterizar.cc")
endif()

idf_component_register(SRCS ${fontes}
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "index.html")
