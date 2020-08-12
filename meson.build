project('pdf-simple-sign', 'cpp', default_options : ['cpp_std=c++11'])
cryptodep = dependency('libcrypto')

executable('pdf-simple-sign', 'pdf-simple-sign.cpp', 'pdf.cpp',
	install : true,
	dependencies : cryptodep)

install_headers('pdf-simple-sign.h')
library('pdf-simple-sign', 'pdf.cpp',
	soversion : 0,
	install : true,
	dependencies : cryptodep)
