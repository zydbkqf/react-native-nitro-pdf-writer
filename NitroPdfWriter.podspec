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

  # Ensure libharu and libpng are downloaded before CocoaPods evaluates the source files.
  s.prepare_command = 'node scripts/prepare-libharu.js'

  s.libraries = 'z'

  s.source_files = [
    'ios/**/*.{h,m,mm,swift}',
    'cpp/**/*.{h,hpp,c,cc,cpp}',
    'libharu/src/**/*.{c,h}',
    'libharu/include/**/*.{h}',
    'libpng/png*.c',
    'libpng/png*.h',
    'nitrogen/generated/shared/c++/**/*.{h,hpp}',
    'nitrogen/generated/ios/c++/**/*.{h,hpp}',
    'nitrogen/generated/ios/swift/**/*.{swift}',
  ]

  s.exclude_files = [
    'cpp/tests',
    'libpng/pngtest.c',
    'libpng/example.c',
  ]

  s.public_header_files = 'ios/**/*.h'

  s.pod_target_xcconfig = {
    'HEADER_SEARCH_PATHS' => [
      '"$(PODS_TARGET_SRCROOT)/cpp"',
      '"$(PODS_TARGET_SRCROOT)/libharu/include"',
      '"$(PODS_TARGET_SRCROOT)/libpng"',
      '"$(PODS_TARGET_SRCROOT)/nitrogen/generated/shared/c++"',
      '"$(PODS_TARGET_SRCROOT)/nitrogen/generated/ios/c++"',
    ].join(' '),
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HPDF_NOPACKPROTOCOL=1 PNG_ARM_NEON_OPT=0',
  }

  s.dependency "React"

  load 'nitrogen/generated/ios/NitroPdfWriter+autolinking.rb'
  add_nitrogen_files(s)
end
