backend_decsync_utils_inc = include_directories(['.'])
backend_decsync_utils_lib = static_library(
  'backend-decsync-utils',
  [
    'backend-decsync-utils.vala'
  ],
  dependencies: [
    gee,
    json_glib,
    libdecsync
  ]
)

backend_decsync_utils = declare_dependency(
  link_with: backend_decsync_utils_lib,
  include_directories: backend_decsync_utils_inc
)

