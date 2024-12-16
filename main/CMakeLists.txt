idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "mesh_main.c"
                            "mesh_netif.c"
                            "mqtt_app.c"
                            "traffic_light.c"
							"ota_app.c"
                    INCLUDE_DIRS "." "include"
                    EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
