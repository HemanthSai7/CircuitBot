idf_component_register(SRCS  "ir_nec_encoder.c" "hello_world_main.c" 
                    INCLUDE_DIRS ""
                    REQUIRES 
                    "esp_https_server"
                    "esp_wifi"
                    "nvs_flash"
                    "json"
                    "driver"
                    "fatfs"
                    "esp_timer"
                    "spiffs"
                    "mdns"

                EMBED_TXTFILES ${project_dir}/main/web_page.htm
                            ${project_dir}/main/icon.png
                            ${project_dir}/main/web_page_2.htm
                            ${project_dir}/main/add_device.htm
                            ${project_dir}/main/control_device.htm
                    )

