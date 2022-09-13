shared_library(
  'ecalbackenddecsync',
  [
    'e-cal-backend-decsync.c',
    'e-cal-backend-decsync.h',
    'e-cal-backend-decsync-events.c',
    'e-cal-backend-decsync-events.h',
    'e-cal-backend-decsync-journal.c',
    'e-cal-backend-decsync-journal.h',
    'e-cal-backend-decsync-todos.c',
    'e-cal-backend-decsync-todos.h',
    'e-cal-backend-decsync-factory.c',
    '../../e-source/e-source-decsync.c',
    '../../e-source/e-source-decsync.h'
  ],
  dependencies: [
    json_glib,
    libdecsync,
    libedatacal
  ],
  install_mode: 'rw-r--r--',
  install: true,
  install_dir: ecal_backenddir,
  include_directories: include_directories(['..', '../..', '../../..'])
)
