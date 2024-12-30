if get_option('swaybar_example_sd_bus_provider') == 'auto'
	sdbus_dep = dependency(['libsystemd', 'libelogind', 'basu',])
else
	sdbus_dep = dependency(get_option('swaybar_example_sd_bus_provider'))
endif

executable(
	'sbar-swaybar',
	'swaybar.c',
	include_directories: inc,
	c_args: [
		'-DHAVE_' + sdbus_dep.name().to_upper(),
		'-DLOG_PREFIX="sbar-swaybar: "',
	],
	dependencies: [sdbus_dep, json_c_dep],
	install: true,
)
