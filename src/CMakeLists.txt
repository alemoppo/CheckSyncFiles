add_library(bv_core STATIC
  Export/CsvExporter.cpp
  Export/ExportUtil.cpp
  Export/JsonExporter.cpp
  Filesystem/FileIndex.cpp
  Filesystem/FileIndexSerializer.cpp
  Filesystem/MftEnumerator.cpp
  Filesystem/PathUtil.cpp
  Filesystem/Win32Enumerator.cpp
  Comparison/FileComparator.cpp
  Hashing/HashCache.cpp
  Hashing/Sha256.cpp
  ScanController.cpp
  Threading/ThreadPool.cpp
)
target_include_directories(bv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bv_core PUBLIC kernel32 bcrypt)

add_executable(bv_cli main_cli.cpp)
target_link_libraries(bv_cli PRIVATE bv_core shell32)

if(BUILD_GUI)
  find_package(SDL3 REQUIRED)
  find_package(SDL3_ttf REQUIRED)
  add_executable(bv_gui
    main_gui.cpp
    UI/AppUI.cpp
    UI/Utf.cpp
  )
  target_link_libraries(bv_gui PRIVATE bv_core SDL3::SDL3 SDL3_ttf::SDL3_ttf ole32 uuid)
  if(MINGW)
    target_link_libraries(bv_gui PRIVATE winmm)
  endif()
endif()
