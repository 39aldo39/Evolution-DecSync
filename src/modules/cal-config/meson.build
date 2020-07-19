shared_library(
  'module-cal-config-decsync',
  [
    'module-cal-config-decsync.c',
    '../../e-source/e-source-decsync.c',
    '../../e-source/e-source-decsync.h',
    '../utils/decsync.c',
    '../utils/decsync.h'
  ],
  name_prefix: '',
  dependencies: [
    jsonc,
    libdecsync,
    libedatacal,
    evolutionshell
  ],
  install_mode: 'rw-r--r--',
  install: true,
  install_dir: evo_moduledir,
  include_directories: include_directories(['..', '../..', '../../..'])
)
