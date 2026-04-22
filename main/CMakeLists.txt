idf_component_register(
    SRCS "main.c" "announcer.c" "lcd_hd44780_i2c.c" "fanfare.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES firebase_client nvs_flash driver
)
