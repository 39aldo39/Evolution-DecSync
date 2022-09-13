shared_library(
  'ebookbackenddecsync',
  [
    'e-book-backend-decsync.c',
    'e-book-backend-decsync.h',
    'e-book-backend-decsync-factory.c',
    '../../e-source/e-source-decsync.c',
    '../../e-source/e-source-decsync.h'
  ],
  dependencies: [
    libdecsync,
    libedatabook
  ],
  c_args: [
    '-DBACKENDDIR="' + ebook_backenddir + '"'
  ],
  install_mode: 'rw-r--r--',
  install: true,
  install_dir: ebook_backenddir,
  include_directories: include_directories(['..', '../..', '../../..',])
)
