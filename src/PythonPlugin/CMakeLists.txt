if(WIN32)
  option(BUILD_PYTHON_PLUGIN "Building Python Plugin" OFF)
else()
  option(BUILD_PYTHON_PLUGIN "Building Python Plugin" ON)
endif()

if(NOT BUILD_PYTHON_PLUGIN)
  return()
elseif(NOT ENABLE_PYTHON)
  message(FATAL_ERROR "PythonPlugin needs to ENABLE_PYTHON")
endif()

set(target CnoidPythonPlugin)

set(sources
  PythonPlugin.cpp
  PythonExecutor.cpp
  PythonScriptItem.cpp
  PythonScriptItemImpl.cpp
  PythonConsoleView.cpp
  )

set(headers
  PythonExecutor.h
  PythonScriptItem.h
  PythonScriptItemImpl.h
  Readline.h
  )

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
  set(sources ${sources} Readline.cpp)
  set(qtheaders Readline.h)
  QT5_WRAP_CPP(qtsources ${qtheaders})
  set(sources ${sources} ${qtsources})
endif()

choreonoid_make_gettext_mo_files(${target} mofiles)
choreonoid_add_plugin(${target} ${sources} ${mofiles} HEADERS ${headers})
if(CMAKE_SYSTEM_NAME STREQUAL Linux)
  target_link_libraries(${target} CnoidBase CnoidPyUtil readline)
else()
  target_link_libraries(${target} CnoidBase CnoidPyUtil )
endif()

configure_file(rbimporter.py ${PROJECT_BINARY_DIR}/${CNOID_PYTHON_SUBDIR}/cnoid/ COPYONLY)
install(FILES rbimporter.py DESTINATION ${CNOID_PYTHON_SUBDIR}/cnoid)
