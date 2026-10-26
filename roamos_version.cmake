# RoamOS version helpers. Run the self-test with:  cmake -P roamos_version.cmake

# "0.1234" -> 1234, "1.0002" -> 10002. Stops the build on anything else
# (0.12, 0.1.0, v0.0001), so a badly written version can never ship.
function(roamos_version_number version out_var)
    if(NOT version MATCHES "^([0-9])\\.([0-9][0-9][0-9][0-9])$")
        message(FATAL_ERROR "RoamOS version \"${version}\" must be one digit, a point and four digits, e.g. 0.0001")
    endif()
    math(EXPR number "${CMAKE_MATCH_1} * 10000 + 1${CMAKE_MATCH_2} - 10000")
    if(number EQUAL 0)
        message(FATAL_ERROR "RoamOS version 0.0000 is not allowed: Matter treats software version 0 as unset")
    endif()
    set(${out_var} ${number} PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    foreach(case "0.0001=1" "0.0120=120" "0.1234=1234" "0.9999=9999" "1.0002=10002" "9.9999=99999")
        string(REPLACE "=" ";" parts "${case}")
        list(GET parts 0 version)
        list(GET parts 1 expected)
        roamos_version_number("${version}" got)
        if(NOT got EQUAL expected)
            message(FATAL_ERROR "${version}: expected ${expected}, got ${got}")
        endif()
    endforeach()
    message(STATUS "roamos_version: all cases pass")
endif()
