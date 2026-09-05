idf_component_register(
        SRCS "board.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_i2s esp_codec_dev echopal_board
)
