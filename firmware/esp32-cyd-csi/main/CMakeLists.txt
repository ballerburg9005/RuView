if(CONFIG_CYD_LCD_I80)
    set(CYD_LCD_SRC "cyd_display_i80.c")
else()
    set(CYD_LCD_SRC "cyd_display.c")
endif()

idf_component_register(
    SRCS "main.c" ${CYD_LCD_SRC}
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer lwip driver esp_lcd
)
