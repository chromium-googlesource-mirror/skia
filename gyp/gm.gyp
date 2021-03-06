# GYP file to build the "gm" (golden master) executable.
{
  'includes': [
    'apptype_console.gypi',
    'common.gypi',
  ],
  'targets': [
    {
      'target_name': 'gm',
      'type': 'executable',
      'includes': [
        'gmslides.gypi',
      ],
      'sources': [
        '../gm/gm.cpp',
        '../gm/gmmain.cpp',
      ],
      'dependencies': [
        'core.gyp:core',
        'effects.gyp:effects',
        'gpu.gyp:gr',
        'gpu.gyp:skgr',
        'images.gyp:images',
        'ports.gyp:ports',
        'pdf.gyp:pdf',
        'utils.gyp:utils',        
      ],
      #mac does not like empty dependency.
      'conditions': [
        [ 'skia_os == "win"', {
          'dependencies': [
            'xps.gyp:xps',
          ],
        }],
      ],  
    },
  ],
}

# Local Variables:
# tab-width:2
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=2 shiftwidth=2:
