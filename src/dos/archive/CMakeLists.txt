# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

# Its own library so the helper process (design D15) links it without the
# DOS layer: nothing from dosboxcommon may go on this link line.
add_library(archivereader STATIC
  ../dos_names.cpp
  archive_activity.cpp
  archive_cache.cpp
  archive_identity.cpp
  archive_lock.cpp
  archive_management.cpp
  archive_reader.cpp
  archive_toml.cpp
  archive_tree.cpp
  cp437.cpp
  cue_rewrite.cpp
  descriptor.cpp
  entry_names.cpp
  exclusive_file.cpp
  head_sniff.cpp
  member_path.cpp
  legacy_zip.cpp
  random_name.cpp
  helper_protocol.cpp
  isz_image.cpp
  rar4_headers.cpp
  sidecar_locator.cpp
  sidecar_record.cpp
  zip_central_directory.cpp
)
target_link_libraries(archivereader
  PUBLIC project_headers augra-log LibArchive::LibArchive ZLIB::ZLIB BZip2::BZip2
  PRIVATE sha256
)
# The engine gets NOMINMAX from dosbox_config.h, which this library does not
# include; libarchive's headers pull in <windows.h> and its min/max macros.
# PUBLIC: a test that includes these headers before the config header
# (archive_images_tests.cpp) needs it too.
target_compile_definitions(archivereader PUBLIC $<$<PLATFORM_ID:Windows>:NOMINMAX>)

# The reader library only: an engine object here is a leak in the reader's closure.
add_executable(dosbox_archive_helper helper_main.cpp)
set_target_properties(dosbox_archive_helper PROPERTIES
  OUTPUT_NAME dosbox-automation-archive-helper
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:dosbox>
)
target_link_libraries(dosbox_archive_helper PRIVATE archivereader)
