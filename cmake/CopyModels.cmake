# Hard-links (or copies) downloaded model files into the build output.
file(GLOB models "${SRC}/*.bin" "${SRC}/*.gguf")
foreach(model ${models})
    get_filename_component(name "${model}" NAME)
    if(NOT EXISTS "${DST}/${name}")
        file(CREATE_LINK "${model}" "${DST}/${name}" COPY_ON_ERROR)
    endif()
endforeach()
