set(WA_PLUGIN_OUT ${WA_OUT_DIR}/plugins)

# Native plugin: builds <id>.dll into bin/plugins/<id>/ next to its manifest.
function(wa_add_native_plugin id)
    add_library(${id} SHARED ${ARGN})
    target_link_libraries(${id} PRIVATE wa_options nlohmann_json::nlohmann_json)
    target_include_directories(${id} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${id} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${WA_PLUGIN_OUT}/${id}
        LIBRARY_OUTPUT_DIRECTORY ${WA_PLUGIN_OUT}/${id})
    add_custom_command(TARGET ${id} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/${id}/manifest.json ${WA_PLUGIN_OUT}/${id}/manifest.json)
endfunction()

# Script plugin: copies its folder into bin/plugins/<id>/.
function(wa_add_script_plugin id)
    file(GLOB files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${id}/*)
    set(outputs)
    foreach(f ${files})
        get_filename_component(name ${f} NAME)
        set(out ${WA_PLUGIN_OUT}/${id}/${name})
        add_custom_command(OUTPUT ${out} COMMAND ${CMAKE_COMMAND} -E copy_if_different ${f} ${out} DEPENDS ${f})
        list(APPEND outputs ${out})
    endforeach()
    add_custom_target(plugin_${id} ALL DEPENDS ${outputs})
endfunction()

wa_add_native_plugin(system_controls system_controls/system_controls.cpp)
target_link_libraries(system_controls PRIVATE ole32 oleaut32 wbemuuid powrprof user32)

wa_add_native_plugin(clipboard_search clipboard_search/clipboard_search.cpp)
target_link_libraries(clipboard_search PRIVATE user32)

wa_add_script_plugin(open_app)
