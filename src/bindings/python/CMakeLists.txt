cmake_minimum_required(VERSION 3.26 FATAL_ERROR)

include(ValhallaVersion)

if (PREFER_EXTERNAL_DEPS)
  find_package(nanobind QUIET CONFIG)
  if (NOT TARGET nanobind::nanobind)
    message(WARNING "No nanobind found in system libraries, using vendored nanobind...")
  endif()
endif()

if (NOT TARGET nanobind::nanobind)
  add_subdirectory(${VALHALLA_SOURCE_DIR}/third_party/nanobind ${CMAKE_BINARY_DIR}/third_party/nanobind EXCLUDE_FROM_ALL)
endif()

message(STATUS "Installing python modules to ${Python_SITEARCH}")

function(set_output_python rel_path)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${rel_path} ${CMAKE_CURRENT_BINARY_DIR}/${rel_path} COPYONLY)
endfunction()

# Generate Python protobuf files
find_package(Protobuf REQUIRED)
set(PROTO_DIR ${VALHALLA_SOURCE_DIR}/proto)
set(PYTHON_PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/valhalla)

# Generate Python protobuf files for incidents
add_custom_command(
  OUTPUT ${PYTHON_PROTO_OUT_DIR}/incidents_pb2.py
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --proto_path=${PROTO_DIR}
       --python_out=${PYTHON_PROTO_OUT_DIR}
       ${PROTO_DIR}/incidents.proto
  DEPENDS ${PROTO_DIR}/incidents.proto
  COMMENT "Generating Python protobuf for incidents"
)

# add and install module(s)
nanobind_add_module(_valhalla 
  NB_STATIC
  STABLE_ABI
  LTO
  ${CMAKE_CURRENT_SOURCE_DIR}/src/_valhalla.cc)
# Note: LTO disabled for _graph_utils to work around GCC 14 ICE on ARM64
# The graph_id.cc file includes heavy baldr/graphreader.h templates that
# trigger "lto1: internal compiler error: in get_token" during LTO phase.
# See: https://github.com/actions/runner/issues/2508
# See: https://www.mail-archive.com/ptxdist@pengutronix.de/msg24413.html
nanobind_add_module(_graph_utils
  NB_STATIC
  STABLE_ABI
  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_utils.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_id.cc)
nanobind_add_module(predicted_speeds
  NB_STATIC
  STABLE_ABI
  LTO
  ${CMAKE_CURRENT_SOURCE_DIR}/src/predicted_speeds.cc)

foreach(tgt IN ITEMS _valhalla _graph_utils predicted_speeds)
  # force abi3.so extension
  nanobind_extension_abi3(${tgt})

  target_link_libraries(${tgt} PRIVATE 
    valhalla
    Python::SABIModule
    $<$<BOOL:${ENABLE_COVERAGE}>:gcov>)
endforeach()

if(APPLE)
  set_target_properties(_valhalla PROPERTIES INSTALL_RPATH "@loader_path")
  set_target_properties(_graph_utils PROPERTIES INSTALL_RPATH "@loader_path")
  set_target_properties(predicted_speeds PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX AND NOT APPLE)
  set_target_properties(_valhalla PROPERTIES INSTALL_RPATH "\$ORIGIN")
  set_target_properties(_graph_utils PROPERTIES INSTALL_RPATH "\$ORIGIN")
  set_target_properties(predicted_speeds PROPERTIES INSTALL_RPATH "\$ORIGIN")
endif()

set_target_properties(_valhalla PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla)
set_target_properties(_graph_utils PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla/utils)
set_target_properties(predicted_speeds PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla/utils)
set_target_properties(valhalla PROPERTIES POSITION_INDEPENDENT_CODE ON)

# # the package we're building, pyvalhalla or pyvalhalla-weekly
# if(NOT VALHALLA_PYTHON_PACKAGE)
#   set(VALHALLA_PYTHON_PACKAGE "pyvalhalla-weekly")
# endif()
# target_compile_definitions(_valhalla PRIVATE -DVALHALLA_PYTHON_PACKAGE=${VALHALLA_PYTHON_PACKAGE})

# message(STATUS "Installing python modules to ${NEVH_Python_SITEARCH}")
# # install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla
# # DESTINATION "${NEVH_Python_SITEARCH}"
# # COMPONENT runtime
# # FILES_MATCHING PATTERN "*.py")
# # install(TARGETS python_valhalla
# # DESTINATION "${NEVH_Python_SITEARCH}/valhalla"
# # COMPONENT runtime)
# # copy valhalla python sources from src to build
# set(py_srcs 
set(py_srcs
  valhalla/__init__.py
  valhalla/__version__.py
  valhalla/config.py
  valhalla/actor.py
  valhalla/utils/__init__.py
  valhalla/utils/graph_utils.py
  valhalla/utils/_graph_utils.pyi
  valhalla/utils/predicted_speeds.pyi
  valhalla/utils/decode_polyline.py)

# scikit-build-core does a different build than CMake
if (SKBUILD)
  # SKBUILD_PLATLIB_DIR: the platlib path inside the wheel (site-packages or dist-packages or so)
  set(VALHALLA_INSTALL_PYDIR "${SKBUILD_PLATLIB_DIR}")

  list(APPEND py_srcs
    valhalla/__main__.py
    valhalla/_scripts.py)

  # install the executables when built with scikit-build-core (not with CMake directly)
  function(install_executable exe)
    install(TARGETS ${exe}
      DESTINATION "${VALHALLA_INSTALL_PYDIR}/valhalla/bin"
      COMPONENT python)
  endfunction()

  if (ENABLE_TOOLS)
    set(tools valhalla_service)
    foreach (exe ${tools})
      install_executable(${exe})
    endforeach()
  endif()

  if (ENABLE_DATA_TOOLS)
    set(data_tools valhalla_build_tiles valhalla_build_admins valhalla_add_predicted_traffic valhalla_add_landmarks valhalla_ingest_transit valhalla_convert_transit)
    foreach (exe ${data_tools})
      install_executable(${exe})
    endforeach()
  endif()
else()
  set(VALHALLA_INSTALL_PYDIR "${Python_SITEARCH}")
  # write the __version__.py
  file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/valhalla/__version__.py"
    "__version__ = \"${VERSION}\"\n")
endif()

# propagate up to the parent
set(Python_FOUND ${Python_FOUND} PARENT_SCOPE)

# copy valhalla python sources from src to build
function(set_output_python rel_path)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${rel_path} ${CMAKE_CURRENT_BINARY_DIR}/${rel_path} COPYONLY)
endfunction()

configure_file(${VALHALLA_SOURCE_DIR}/scripts/valhalla_build_config ${CMAKE_CURRENT_BINARY_DIR}/valhalla/valhalla_build_config.py COPYONLY)

foreach (py_src ${py_srcs})
  set_output_python(${py_src})
endforeach()

# Add the generated protobuf file as a dependency
add_custom_target(generate_incidents_pb2 ALL DEPENDS ${PYTHON_PROTO_OUT_DIR}/incidents_pb2.py)

install(TARGETS _valhalla
#   DESTINATION "${NEVH_Python_SITEARCH}"
#   COMPONENT runtime)
# install(TARGETS graph_utils
#   DESTINATION "${NEVH_Python_SITEARCH}/utils"
#   COMPONENT runtime)
# install(FILES ${PYTHON_PROTO_OUT_DIR}/incidents_pb2.py
#   DESTINATION "${NEVH_Python_SITEARCH}/valhalla"
#   COMPONENT runtime)

# install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla ${CMAKE_CURRENT_BINARY_DIR}/valhalla/utils
#   DESTINATION "${NEVH_Python_SITEARCH}"
#   COMPONENT runtime
#   FILES_MATCHING PATTERN "*.py*")
  DESTINATION "${VALHALLA_INSTALL_PYDIR}/valhalla"
  COMPONENT python)
install(TARGETS _graph_utils
  DESTINATION "${VALHALLA_INSTALL_PYDIR}/valhalla/utils"
  COMPONENT python)
install(TARGETS predicted_speeds
  DESTINATION "${VALHALLA_INSTALL_PYDIR}/valhalla/utils"
  COMPONENT python)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/valhalla
  DESTINATION "${VALHALLA_INSTALL_PYDIR}"
  COMPONENT python
  FILES_MATCHING
    PATTERN "*.py*"
    PATTERN "__pycache__" EXCLUDE)
