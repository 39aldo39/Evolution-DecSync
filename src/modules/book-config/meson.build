shared_library(
  'module-book-config-decsync',
  [
    'module-book-config-decsync.c',
    '../../e-source/e-source-decsync.c',
    '../../e-source/e-source-decsync.h',
    '../utils/decsync.c',
    '../utils/decsync.h'
  ],
  name_prefix: '',
  dependencies: [
    json_glib,
    libdecsync,
    libedatabook,
    evolutionshell
  ],
  install_mode: 'rw-r--r--',
  install: true,
  install_dir: evo_moduledir,
  include_directories: include_directories(['..', '../..', '../../..'])
)
