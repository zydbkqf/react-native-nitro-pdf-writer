require 'json'

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name = 'NitroPdfWriter'
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => "15.1" }
  s.source = { :git => 'https://github.com/zydbkqf/react-native-nitro-pdf-writer.git', :tag => "#{s.version}" }

  s.static_framework = true

  # libharu and libpng are vendored under thirdparty/ and compiled from source
  # for every platform — no download step runs during `pod install`.

  s.libraries = 'z'
  # ImageIO decodes HEIC/WebP/AVIF before the bytes reach libharu (PDF cannot
  # embed those containers); CoreGraphics normalizes them to sRGB. CoreImage is
  # only used to bake EXIF orientation when ImageIO refuses the thumbnail path.
  s.frameworks = 'ImageIO', 'CoreGraphics', 'CoreImage'

  # The transcoder registers itself from `+load`; without -ObjC the linker drops
  # that object file out of the static framework and HEIC/WebP stop working.
  s.user_target_xcconfig = { 'OTHER_LDFLAGS' => '$(inherited) -ObjC' }

  s.source_files = [
    'ios/**/*.{h,m,mm,swift}',
    'cpp/**/*.{h,hpp,c,cc,cpp}',
    'thirdparty/libharu/src/**/*.{c,h}',
    'thirdparty/libharu/include/**/*.{h}',
    'thirdparty/libpng/png*.c',
    'thirdparty/libpng/png*.h',
    'nitrogen/generated/shared/c++/**/*.{h,hpp}',
    'nitrogen/generated/ios/c++/**/*.{h,hpp}',
    'nitrogen/generated/ios/swift/**/*.{swift}',
  ]

  s.exclude_files = [
    'cpp/tests',
    'thirdparty/libpng/pngtest.c',
    'thirdparty/libpng/example.c',
  ]

  s.public_header_files = 'ios/**/*.h'

  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/cpp"',
      '"$(PODS_TARGET_SRCROOT)/thirdparty/libharu/include"',
      '"$(PODS_TARGET_SRCROOT)/thirdparty/libpng"',
      '"$(PODS_TARGET_SRCROOT)/nitrogen/generated/shared/c++"',
      '"$(PODS_TARGET_SRCROOT)/nitrogen/generated/ios/c++"',
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HPDF_NOPACKPROTOCOL=1 PNG_ARM_NEON_OPT=0',
  }

  s.dependency "React"

  load 'nitrogen/generated/ios/NitroPdfWriter+autolinking.rb'
  add_nitrogen_files(s)
end
