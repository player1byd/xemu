
testblock = declare_dependency(dependencies: [block], sources: 'iothread.c')

xbox_i386_target_headers = declare_dependency(
  include_directories: include_directories('../../target/i386'),
  compile_args: [
    '-DCOMPILING_PER_TARGET',
    '-DCONFIG_TARGET="i386-softmmu-config-target.h"',
    '-DCONFIG_DEVICES="i386-softmmu-config-devices.h"',
  ],
)

tests = {
  'check-block-qdict': [],
  'check-qdict': [],
  'check-qnum': [],
  'check-qstring': [],
  'check-qlist': [],
  'check-qnull': [],
  'check-qobject': [],
  'check-qjson': [],
  'check-qlit': [],
  'test-error-report': [],
  'test-qobject-output-visitor': [testqapi],
  'test-clone-visitor': [testqapi],
  'test-qobject-input-visitor': [testqapi],
  'test-forward-visitor': [testqapi],
  'test-string-input-visitor': [testqapi],
  'test-string-output-visitor': [testqapi],
  'test-visitor-serialization': [testqapi],
  'test-bitmap': [],
  'test-xbox-pgraph-texture-state': [],
  'test-xbox-pgraph-texture-layout': [],
  'test-xbox-pgraph-uniform-dirty': [xbox_i386_target_headers],
  'test-xbox-pgraph-uniform-source': [],
  'test-xbox-pgraph-renderer-switch-locking': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/renderer-switch.c',
  ],
  'test-xbox-vk-debug-macros': [],
  'test-xbox-nv2a-profile-event-log': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/profile.c',
    xbox_i386_target_headers,
  ],
  'test-xbox-pgraph-reports': [
    'xbox-pgraph-report-test-support.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/reports.c',
    xbox_i386_target_headers,
  ],
  'test-xbox-pgraph-report-wrapper': [
    'xbox-pgraph-report-test-support.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/reports.c',
    xbox_i386_target_headers,
  ],
  'test-xbox-vk-blend-constants': [],
  'test-xbox-vk-texture-binding': [],
  'test-xbox-vk-display-output-state': [],
  'test-xemu-win32-dxgi-interop': [],
  'test-xbox-vk-device-selection': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/device-selection.c',
  ],
  'test-xbox-vk-device-inventory': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/device-inventory.c',
  ],
  'test-xemu-gpu-launch': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/device-selection.c',
    meson.project_source_root() / 'ui/xemu-gpu-launch.c',
  ],
  'test-xemu-gpu-info': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/device-selection.c',
    meson.project_source_root() / 'ui/xemu-gpu-launch.c',
    meson.project_source_root() / 'ui/xemu-gpu-info.c',
  ],
  'test-xbox-vk-bc-layout': [],
  'test-xbox-pgraph-vertex-fetch-span': [],
  'test-xbox-gl-draw-lifecycle': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/gl/draw-lifecycle.c',
    xbox_i386_target_headers, gloffscreen, epoxy,
  ],
  'test-xbox-gl-texture-binding': [],
  'test-xbox-vk-failure-state': [],
  'test-xbox-vk-surface-coherence': [],
  'test-xbox-vk-failpoint': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/failpoint.c',
    declare_dependency(
      compile_args: ['-DXEMU_VK_DIAGNOSTIC_FAILURES'],
    ),
  ],
  'test-xbox-vk-pipeline-lifetime': [],
  'test-xbox-vk-pipeline-cache-data': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/pipeline-cache-data.c',
  ],
  'test-xbox-vk-vertex-version-policy': [],
  'test-xbox-vk-ubershader-controls': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/ubershader-controls.c',
  ],
  'test-xbox-vk-ubershader-combiner': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/ubershader-controls.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/ubershader-combiner-oracle.c',
  ],
  'test-xbox-vk-ubershader-glsl': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/common.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh-uber.c',
    xbox_i386_target_headers,
  ],
  'test-xbox-vk-hybrid-policy': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-policy.c',
  ],
  'test-xbox-vk-hybrid-compiler': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-compiler.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-policy.c',
  ],
  'test-xbox-vk-hybrid-pipeline-builder': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-pipeline-builder.c',
  ],
  'test-xbox-vk-hybrid-trace': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-trace.c',
  ],
  'test-xbox-vk-uniform-layout': [],
  'test-xbox-vk-spirv-prewarm': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/spirv-prewarm.c',
  ],
  'test-xbox-vk-hybrid-family-history': [
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-history.c',
  ],
  'test-xbox-nv2a-ptimer': [
    'ptimer-test-stubs.c',
    meson.project_source_root() / 'hw/xbox/nv2a/ptimer.c',
    meson.project_source_root() / 'hw/xbox/nv2a/ptimer_core.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pramdac.c',
    declare_dependency(
      compile_args: [
        '-include',
        meson.project_source_root() / 'tests/unit/xbox-nv2a-ptimer-test-shim.h',
      ],
    ),
  ],
  'test-resv-mem': [],
  # all code tested by test-x86-topo is inside topology.h
  'test-x86-topo': [],
  'test-cutils': [],
  'test-div128': [],
  'test-shift128': [],
  'test-mul64': [],
  # all code tested by test-int128 is inside int128.h
  'test-int128': [],
  'rcutorture': [],
  'test-rcu-list': [],
  'test-rcu-simpleq': [],
  'test-rcu-tailq': [],
  'test-rcu-slist': [],
  'test-qdist': [],
  'test-qht': [],
  'test-qtree': [],
  'test-bitops': [],
  'test-bitcnt': [],
  'test-qgraph': ['../qtest/libqos/qgraph.c'],
  'check-qom-interface': [qom],
  'check-qom-proplist': [qom],
  'test-qemu-opts': [],
  'test-keyval': [testqapi],
  'test-logging': [],
  'test-qapi-util': [],
  'test-interval-tree': [],
  'test-fifo': [],
  'test-lru-active': [],
  'test-xbox-mcpx-apu-resampler': [libsamplerate],
}

if host_os == 'windows'
  tests += {
    'test-xbox-qemu-poll': [],
  }
endif

if vulkan.found()
  tests += {
    'test-xbox-vk-hybrid-prewarm': [
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-prewarm.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-prewarm-runtime.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-codec.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-history.c',
      xbox_i386_target_headers, volk, libglslang, vma, vulkan, spirv_reflect,
    ],
    'test-xbox-vk-staging-flush': [volk, vma],
    'test-xbox-vk-hybrid-family-codec': [
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-codec.c',
      xbox_i386_target_headers, volk, libglslang, vma, vulkan, spirv_reflect,
    ],
    'test-xbox-vk-ubershader-glsl-integration': [
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/common.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/geom.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh-uber.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/vsh-prog.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/device-inventory.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/glsl.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-compiler.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-policy.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/spirv-prewarm.c',
      xbox_i386_target_headers,
      volk, libglslang, vma, vulkan, spirv_reflect,
    ],
  }

  # Retain the production candidate resolver and fallback-family ownership
  # call chains while allowing the linker to discard the rest of the large
  # renderer translation units.
  if 'i386-softmmu' in target_dirs
    nv2a_vk_runtime_section_args = cc.get_supported_arguments(
      '-ffunction-sections',
      '-fdata-sections',
    )
    if host_os == 'darwin'
      nv2a_vk_runtime_gc_link_args = cc.get_supported_link_arguments(
        '-Wl,-dead_strip',
      )
    else
      nv2a_vk_runtime_gc_link_args = cc.get_supported_link_arguments(
        '-Wl,--gc-sections',
      )
    endif
    if nv2a_vk_runtime_section_args.length() == 2 and \
       nv2a_vk_runtime_gc_link_args.length() == 1
      nv2a_vk_runtime_prod = declare_dependency(
        sources: config_target_h['i386-softmmu'],
        compile_args: [
          '-DCOMPILING_PER_TARGET',
          '-DCONFIG_TARGET="i386-softmmu-config-target.h"',
        ] + nv2a_vk_runtime_section_args,
        link_args: nv2a_vk_runtime_gc_link_args,
        include_directories: include_directories('../../target/i386'),
      )
      tests += {
        'test-xbox-vk-hybrid-wake': [
          meson.project_source_root() / 'hw/xbox/nv2a/pfifo.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-wake.c',
          nv2a_vk_runtime_prod,
          xbox_i386_target_headers, volk, libglslang, vma, vulkan,
          spirv_reflect,
        ],
        'test-xbox-vk-ubershader-runtime': [
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/common.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/geom.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-codec.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-family-history.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-trace.c',
          meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/ubershader-controls.c',
          nv2a_vk_runtime_prod,
          xbox_i386_target_headers, volk, libglslang, vma, vulkan,
          spirv_reflect,
        ],
      }
    else
      message('Skipping NV2A Vulkan runtime integration test: section GC unsupported')
    endif
  endif
endif

# This integration unit retains only the production pending-report, finish,
# and report-publication call chains from large target-specific translation
# units. Section GC keeps the test link closure bounded without replacing the
# queue or retirement logic with test-only helpers.
if 'i386-softmmu' in target_dirs and vulkan.found()
  nv2a_vk_report_section_args = cc.get_supported_arguments(
    '-ffunction-sections',
    '-fdata-sections',
  )
  if host_os == 'darwin'
    nv2a_vk_report_gc_link_args = cc.get_supported_link_arguments(
      '-Wl,-dead_strip',
    )
  else
    nv2a_vk_report_gc_link_args = cc.get_supported_link_arguments(
      '-Wl,--gc-sections',
    )
  endif

  if nv2a_vk_report_section_args.length() == 2 and \
     nv2a_vk_report_gc_link_args.length() == 1
    nv2a_vk_report_test = declare_dependency(
      sources: config_target_h['i386-softmmu'],
      compile_args: [
        '-DCOMPILING_PER_TARGET',
        '-DCONFIG_TARGET="i386-softmmu-config-target.h"',
        '-DXBOX_PGRAPH_REPORT_TEST_USE_PRODUCTION_DMA_LOAD',
        '-include',
        meson.project_source_root() / 'tests/unit/xbox-nv2a-report-test-shim.h',
      ] + nv2a_vk_report_section_args,
      link_args: nv2a_vk_report_gc_link_args,
      include_directories: include_directories('../../target/i386'),
    )
    nv2a_vk_report_test_headers = [
      volk.partial_dependency(compile_args: true, includes: true),
      libglslang.partial_dependency(compile_args: true, includes: true),
      vma.partial_dependency(compile_args: true, includes: true),
      vulkan.partial_dependency(compile_args: true, includes: true),
      spirv_reflect,
    ]
    tests += {
      'test-xbox-pgraph-vk-reports': [
        'xbox-pgraph-report-test-support.c',
        meson.project_source_root() / 'hw/xbox/nv2a/nv2a.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/pgraph.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/reports.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/command.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/draw.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/glsl.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-policy.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/hybrid-trace.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/perf.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/reports.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/vk/surface-compute.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/profile.c',
        nv2a_vk_report_test,
        nv2a_vsh_cpu.partial_dependency(compile_args: true, includes: true),
      ] + nv2a_vk_report_test_headers,
    }
  else
    message('Skipping NV2A Vulkan report integration test: section GC unsupported')
  endif
endif

# This real graphics lifecycle test skips unless explicitly enabled on a host
# with an interactive SDL/OpenGL session.
if sdl.found() and epoxy.found()
  tests += {
    'test-xbox-gloffscreen': [
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/sdl.c',
      meson.project_source_root() / 'hw/xbox/nv2a/pgraph/gl/texture-stage.c',
      sdl, epoxy,
    ],
  }
  if 'i386-softmmu' in target_dirs
    tests += {
      'test-xbox-psh-depth-replace': [
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/common.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh.c',
        meson.project_source_root() / 'hw/xbox/nv2a/pgraph/glsl/psh-uber.c',
        xbox_i386_target_headers, sdl, epoxy,
      ],
    }
  endif
endif

if sdl.found()
  tests += {'test-xemu-input-topology': [
    meson.project_source_root() / 'ui/xemu-input-topology.c',
    sdl,
  ]}
endif

if have_system or have_tools
  tests += {
    'test-qmp-event': [testqapi],
  }

  if seccomp.found()
    tests += {'test-seccomp': ['../../system/qemu-seccomp.c', seccomp]}
  endif
endif

if have_block
  tests += {
    'test-coroutine': [testblock],
    'test-aio': [testblock],
    'test-aio-multithread': [testblock],
    'test-throttle': [testblock],
    'test-thread-pool': [testblock],
    'test-hbitmap': [testblock],
    'test-bdrv-drain': [testblock],
    'test-bdrv-graph-mod': [testblock],
    'test-blockjob': [testblock],
    'test-blockjob-txn': [testblock],
    'test-block-backend': [testblock],
    'test-block-iothread': [testblock],
    'test-write-threshold': [testblock],
    'test-crypto-hash': [crypto],
    'test-crypto-hmac': [crypto],
    'test-crypto-cipher': [crypto],
    'test-crypto-akcipher': [crypto],
    'test-crypto-secret': [crypto, keyutils],
    'test-crypto-der': [crypto],
    'test-authz-simple': [authz],
    'test-authz-list': [authz],
    'test-authz-listfile': [authz],
    'test-io-task': [testblock],
    'test-io-channel-socket': ['socket-helpers.c', 'io-channel-helpers.c', io],
    'test-io-channel-file': ['io-channel-helpers.c', io],
    'test-io-channel-command': ['io-channel-helpers.c', io],
    'test-io-channel-buffer': ['io-channel-helpers.c', io],
    'test-io-channel-null': [io],
    'test-crypto-ivgen': [io],
    'test-crypto-afsplit': [io],
    'test-crypto-block': [io],
    'test-timed-average': [],
    'test-uuid': [],
  }
  if gnutls.found() and \
     tasn1.found() and \
     host_os != 'windows'
    tests += {
      'test-crypto-tlscredsx509': ['crypto-tls-x509-helpers.c',
                                   tasn1, crypto, gnutls],
      'test-crypto-tlssession': ['crypto-tls-x509-helpers.c', 'crypto-tls-psk-helpers.c',
                                 tasn1, crypto, gnutls],
      'test-io-channel-tls': ['io-channel-helpers.c', 'crypto-tls-x509-helpers.c',
                              tasn1, io, crypto, gnutls]}
  endif
  if pam.found()
    tests += {'test-authz-pam': [authz]}
  endif
  if host_os != 'windows'
    tests += {
      'test-image-locking': [testblock],
      'test-nested-aio-poll': [],
    }
  endif
  if config_host_data.get('CONFIG_REPLICATION')
    tests += {'test-replication': [testblock]}
  endif
  tests += {'test-crypto-pbkdf': [io]}
endif

if have_system
  tests += {
    'ptimer-test': ['ptimer-test-stubs.c', meson.project_source_root() / 'hw/core/ptimer.c'],
    'test-iov': [],
    'test-opts-visitor': [testqapi],
    'test-xs-node': [qom],
    'test-virtio-dmabuf': [meson.project_source_root() / 'hw/display/virtio-dmabuf.c'],
    'test-qmp-cmds': [testqapi],
    'test-xbzrle': [migration],
    'test-util-sockets': ['socket-helpers.c'],
    'test-base64': [],
    'test-bufferiszero': [],
    'test-smp-parse': [qom, meson.project_source_root() / 'hw/core/machine-smp.c'],
    'test-vmstate': [migration, io],
    'test-yank': ['socket-helpers.c', qom, io, chardev]
  }
  if config_host_data.get('CONFIG_INOTIFY1')
    tests += {'test-util-filemonitor': []}
  endif

  # Some tests: test-char, test-qdev-global-props, and test-qga,
  # are not runnable under TSan due to a known issue.
  # https://github.com/google/sanitizers/issues/1116
  if not get_option('tsan')
    if host_os != 'windows'
        tests += {
          'test-char': ['socket-helpers.c', qom, io, chardev]
        }
    endif

    tests += {
      'test-qdev-global-props': [qom, hwcore]
    }
  endif
endif

if have_ga and host_os == 'linux'
  tests += {'test-qga': ['../qtest/libqmp.c']}
  test_deps += {'test-qga': qga}
endif

test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())

# This test includes the private production upload implementation. Discard
# unrelated renderer entry points rather than replacing its converters/GL calls.
if host_os == 'linux' and epoxy.found()
  gl_upload_test = executable('test-xbox-gl-texture-upload',
    files('test-xbox-gl-texture-upload.c',
          '../../hw/xbox/nv2a/pgraph/texture.c',
          '../../hw/xbox/nv2a/pgraph/swizzle.c',
          '../../hw/xbox/nv2a/pgraph/s3tc.c'), genh,
    dependencies: [qemuutil, xbox_i386_target_headers, epoxy],
    include_directories: include_directories(
      '../../hw/xbox/nv2a/pgraph/thirdparty/gloffscreen'),
    c_args: ['-ffunction-sections', '-fdata-sections'],
    link_args: ['-Wl,--gc-sections'])
  gl_upload_env = environment()
  gl_upload_env.set('EGL_PLATFORM', 'surfaceless')
  gl_upload_env.set('LIBGL_ALWAYS_SOFTWARE', '1')
  test('test-xbox-gl-texture-upload', gl_upload_test,
       env: gl_upload_env, args: ['--tap', '-k'], protocol: 'tap', suite: ['unit'])
endif

slow_tests = {
  'test-aio-multithread' : 120,
  'test-bufferiszero': 60,
  'test-crypto-block' : 300,
  'test-crypto-tlscredsx509': 90,
  'test-crypto-tlssession': 90,
  'test-replication': 60,
}

xemu_tweaks_test = executable('test-xemu-tweaks-config',
  files('test-xemu-tweaks-config.cc', '../../ui/xemu-tweaks.c',
        '../../ui/xemu-settings-migration.cc'), genh,
  dependencies: [qemuutil, genconfig, tomlplusplus])
test('test-xemu-tweaks-config', xemu_tweaks_test, suite: ['unit'])

test('test-xemu-version', find_program('test-xemu-version.sh'),
     args: [meson.project_source_root() / 'scripts/xemu-version.sh'],
     suite: ['unit'])

# This driver compiles small native API doubles, even during a cross build.
if build_machine.system() in ['linux', 'darwin']
  tweak_test_cc = find_program('cc', native: true, required: false)
  if tweak_test_cc.found()
    test('test-xemu-tweak-paths', python,
      args: [files('test-xemu-tweak-paths.py'), '--cc', tweak_test_cc.full_path()],
      suite: ['unit'])
  endif
endif

foreach test_name, extra: tests
  src = [test_name + '.c']
  deps = [qemuutil]
  if extra.length() > 0
    # use a sourceset to quickly separate sources and deps
    test_ss = ss.source_set()
    test_ss.add(extra)
    src += test_ss.all_sources()
    deps += test_ss.all_dependencies()
  endif
  exe = executable(test_name, src, genh, dependencies: deps)

  test(test_name, exe,
       depends: test_deps.get(test_name, []),
       env: test_env,
       args: ['--tap', '-k'],
       protocol: 'tap',
       timeout: slow_tests.get(test_name, 30),
       priority: slow_tests.get(test_name, 30),
       suite: ['unit'])
endforeach
