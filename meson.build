project('evolution-decsync', ['c'],
  version: '2.1.0-3-evolution-3.44',
  meson_version: '>= 0.44.1',
  license: 'LGPL',
  default_options: ['prefix=/usr']
)

# Dependencies
libedatabook   = dependency('libedata-book-1.2', version: '>=3.44')
libedatacal    = dependency('libedata-cal-2.0', version: '>=3.44')
evolutionshell = dependency('evolution-shell-3.0', version: '>=3.44')
json_glib      = dependency('json-glib-1.0')
libdecsync     = dependency('decsync', version: '>=2.0.1')

# Special directories
LIB_INSTALL_DIR      = join_paths(get_option('prefix'), 'lib')
SHARE_INSTALL_PREFIX = join_paths(get_option('prefix'), 'share')

privlibdir   = join_paths(LIB_INSTALL_DIR, meson.project_name())
moduledir    = join_paths(privlibdir, 'registry-modules')
privsharedir = join_paths(SHARE_INSTALL_PREFIX, meson.project_name())

E_SOURCE_DIR = join_paths(privsharedir, 'sources')

ebook_backenddir = libedatabook.get_pkgconfig_variable('backenddir')
ecal_backenddir  = libedatacal.get_pkgconfig_variable('backenddir')
evo_moduledir    = evolutionshell.get_pkgconfig_variable('moduledir')

# Generate the ${PROJECT_NAME}-config.h file
conf_data = configuration_data()
conf_data.set('PROJECT_NAME', meson.project_name())
conf_data.set('E_SOURCE_DIR', E_SOURCE_DIR)
configure_file(
  input: 'config.h.in',
  output: meson.project_name() + '-config.h',
  configuration: conf_data
)

subdir('src')
