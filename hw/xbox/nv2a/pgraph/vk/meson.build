specific_ss.add(files('device-selection.c'))

if vulkan.found()

specific_ss.add([volk, libglslang, vma, vulkan, spirv_reflect, 
	files(
		'blit.c',
		'buffer.c',
		'command.c',
		'debug.c',
		'device-inventory.c',
		'display.c',
		'draw.c',
		'failpoint.c',
		'glsl.c',
		'gpuprops.c',
		'hybrid-compiler.c',
		'hybrid-family.c',
		'hybrid-family-codec.c',
		'hybrid-family-history.c',
		'hybrid-pipeline-builder.c',
		'hybrid-prewarm.c',
		'hybrid-prewarm-runtime.c',
		'hybrid-trace.c',
		'hybrid-wake.c',
		'hybrid-policy.c',
		'image.c',
		'instance.c',
		'perf.c',
		'pipeline-cache-data.c',
		'renderer.c',
		'reports.c',
		'spirv-prewarm.c',
		'shaders.c',
		'surface-compute.c',
		'surface.c',
		'texture.c',
		'ubershader-controls.c',
		'vertex.c',
		)
	])

endif
