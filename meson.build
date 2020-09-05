project('pdf-simple-sign', 'cpp', default_options : ['cpp_std=c++11'],
	version : '1.0')

conf = configuration_data()
conf.set('PROJECT_NAME', '"' + meson.project_name() + '"')
conf.set('PROJECT_VERSION', '"' + meson.project_version() + '"')
configure_file(output : 'config.h', configuration : conf)

cryptodep = dependency('libcrypto')
executable('pdf-simple-sign', 'pdf-simple-sign.cpp',
	install : true,
	dependencies : cryptodep)
