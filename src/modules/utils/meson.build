decsync_utils_inc = include_directories(['.'])
decsync_utils_lib = static_library(
  'decsync-utils',
  [
    'decsync-utils.vala'
  ],
  dependencies: [
    gee,
    json_glib,
    libdecsync
  ]
)

decsync_utils = declare_dependency(
  link_with: decsync_utils_lib,
  include_directories: decsync_utils_inc
)

