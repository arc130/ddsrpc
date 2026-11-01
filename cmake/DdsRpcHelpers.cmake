include(CMakeParseArguments)
include("${CMAKE_CURRENT_LIST_DIR}/ScanIdlImports.cmake")

function(ddsrpc_add_idl target)
  set(options)
  set(one_value_args NAMESPACE)
  set(multi_value_args ROLES IDL INCLUDE_DIRS)
  cmake_parse_arguments(DDSRPC "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT DDSRPC_IDL)
    message(FATAL_ERROR "ddsrpc_add_idl(${target}) requires IDL")
  endif()
  if(NOT DDSRPC_ROLES)
    set(DDSRPC_ROLES service)
  endif()

  set(include_dirs_abs "")
  foreach(dir IN LISTS DDSRPC_INCLUDE_DIRS)
    get_filename_component(dir_abs "${dir}" ABSOLUTE)
    list(APPEND include_dirs_abs "${dir_abs}")
  endforeach()

  set(all_files "")
  set(root_abs_args "")
  foreach(root IN LISTS DDSRPC_IDL)
    get_filename_component(root_abs "${root}" ABSOLUTE)
    list(APPEND root_abs_args "${root_abs}")
    set(scanned "")
    ddsrpc_scan_idl_imports("${root_abs}" "${include_dirs_abs}" scanned)
    list(APPEND all_files ${scanned})
  endforeach()
  list(REMOVE_DUPLICATES all_files)

  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/ddsrpc/${target}")
  set(outputs "")
  set(seen_stems "")
  foreach(idl IN LISTS all_files)
    get_filename_component(stem "${idl}" NAME_WE)
    if(stem IN_LIST seen_stems)
      message(FATAL_ERROR "Duplicate IDL stem '${stem}' in ${target}")
    endif()
    list(APPEND seen_stems "${stem}")
    list(APPEND outputs "${gen_dir}/${stem}.ddsrpc.types.h")

    list(FIND DDSRPC_ROLES "service" service_index)
    if(service_index GREATER -1)
      list(APPEND outputs
        "${gen_dir}/${stem}.ddsrpc.service.h"
        "${gen_dir}/${stem}.ddsrpc.service.cpp")
    endif()
    list(FIND DDSRPC_ROLES "client" client_index)
    if(client_index GREATER -1)
      list(APPEND outputs
        "${gen_dir}/${stem}.ddsrpc.client.h"
        "${gen_dir}/${stem}.ddsrpc.client.cpp")
    endif()
  endforeach()

  string(REPLACE ";" "_" role_tag "${DDSRPC_ROLES}")
  set(depfile "${gen_dir}/ddsrpc-${role_tag}.d")
  set(role_arg "all")
  list(LENGTH DDSRPC_ROLES role_count)
  if(role_count EQUAL 1)
    set(role_arg "${DDSRPC_ROLES}")
  endif()

  set(ns_args "")
  if(DEFINED DDSRPC_NAMESPACE AND NOT DDSRPC_NAMESPACE STREQUAL "")
    set(ns_args --namespace "${DDSRPC_NAMESPACE}")
  endif()

  set(include_args "")
  foreach(dir IN LISTS include_dirs_abs)
    list(APPEND include_args -I "${dir}")
  endforeach()

  add_custom_command(
    OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND $<TARGET_FILE:ddsrpcgen>
            --role "${role_arg}"
            --out-dir "${gen_dir}"
            ${ns_args}
            ${include_args}
            --depfile "${depfile}"
            ${root_abs_args}
    DEPENDS ddsrpcgen ${all_files}
    BYPRODUCTS "${depfile}"
    COMMENT "Generating ddsrpc code for ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} PRIVATE "${gen_dir}")
  target_link_libraries(${target} PUBLIC ddsrpc::runtime)
  set_target_properties(${target} PROPERTIES AUTOMOC ON)
endfunction()
