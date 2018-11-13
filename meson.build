project('evolution-decsync', ['vala', 'c'],
  version: '1.0',
  meson_version: '>= 0.44.1',
  license: 'GPL3+',
  default_options: ['prefix=/usr']
)

# Dependencies
gee            = dependency('gee-0.8')
json_glib      = dependency('json-glib-1.0')
libedatabook   = dependency('libedata-book-1.2')
libedatacal    = dependency('libedata-cal-1.2')
evolutionshell = dependency('evolution-shell-3.0')
libdecsync     = subproject('libdecsync').get_variable('libdecsync')

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
