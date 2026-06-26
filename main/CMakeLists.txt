idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    EMBED_FILES "audio_assets/custom_audio.wav")

add_custom_target(check_custom_audio ALL
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/check_custom_audio.py"
            "${CMAKE_CURRENT_LIST_DIR}/audio_assets/custom_audio.wav"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/audio_assets/custom_audio.wav"
    VERBATIM)

add_dependencies(${COMPONENT_LIB} check_custom_audio)

set(EXTRA_COMPONENT_DIRS "../../../components")

target_compile_options(${COMPONENT_LIB} INTERFACE -Wno-missing-field-initializers)

# Function to get component library
function(get_component_library component_name output_var)
    # Get the exact component name
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    # Get the component library
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

get_component_library("lvgl" LVGL_LIB)
target_compile_options(
    ${LVGL_LIB}
    PUBLIC
        "-Wno-attributes"
)
