# env, not CONFIG_: requirements are resolved before sdkconfig exists
if(NOT "$ENV{AUTOTERM_VARIANT}" MATCHES "^(matter|all)")
    idf_component_register()
else()
    idf_component_register(SRCS "at_matter.cpp"
                           INCLUDE_DIRS "include"
                           REQUIRES at_app esp_matter)
endif()
