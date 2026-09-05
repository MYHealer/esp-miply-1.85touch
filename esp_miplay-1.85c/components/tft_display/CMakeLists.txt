idf_component_register(SRCS "tft_display.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_timer echopal_board esp_lcd_st77916)
