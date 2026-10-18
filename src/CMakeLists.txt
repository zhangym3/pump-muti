idf_component_register(SRCS "main.cpp" "protocol.cpp"
  "pump_safety_controller.cpp" "master_scheduler.cpp" "hardware.cpp" "storage.cpp"
  "mesh_transport.cpp" "mesh_management.cpp" "control_gateway.cpp" "application.cpp"
  INCLUDE_DIRS "../include" REQUIRES bt nvs_flash esp_driver_gpio esp_driver_uart esp_driver_ledc esp_timer esp_hw_support esp_system)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
