#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <string>

#include "PlatformImageTranscoder.hpp"

using namespace margelo::nitro::pdfwriter;

namespace {

/**
 * Render `image` into an sRGB 8-bit RGBA bitmap of at most `maxDimension`
 * pixels on its long edge.
 *
 * Two reasons this normalization step is not optional:
 *  - HEIC may carry a CMYK or wide-gamut profile; PDF image XObjects built by
 *    libharu expect plain RGB or gray, and a 4-component JPEG would land in
 *    DeviceCMYK with the wrong rendering intent.
 *  - It also bounds memory: a 48 MP photo never allocates a 192 MB buffer.
 *
 * Orientation is already handled upstream: the caller uses the ImageIO
 * thumbnail API with `kCGImageSourceCreateThumbnailWithTransform`, which bakes
 * EXIF orientation into the returned image. PDF has no orientation concept, so
 * this is the only place that can fix a portrait photo.
 */
CGImageRef createScaledSRGBImage(CGImageRef image, uint32_t maxDimension, bool& hasAlpha) {
  const size_t sourceWidth = CGImageGetWidth(image);
  const size_t sourceHeight = CGImageGetHeight(image);
  if (sourceWidth == 0 || sourceHeight == 0) {
    return nullptr;
  }

  const size_t longest = sourceWidth > sourceHeight ? sourceWidth : sourceHeight;
  double scale = 1.0;
  if (maxDimension > 0 && longest > maxDimension) {
    scale = static_cast<double>(maxDimension) / static_cast<double>(longest);
  }
  const size_t width = static_cast<size_t>(sourceWidth * scale);
  const size_t height = static_cast<size_t>(sourceHeight * scale);
  if (width == 0 || height == 0) {
    return nullptr;
  }

  CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
  hasAlpha = alpha != kCGImageAlphaNone && alpha != kCGImageAlphaNoneSkipLast &&
             alpha != kCGImageAlphaNoneSkipFirst;

  CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  if (colorSpace == nullptr) {
    colorSpace = CGColorSpaceCreateDeviceRGB();
  }
  CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, width * 4, colorSpace,
                                               kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(colorSpace);
  if (context == nullptr) {
    return nullptr;
  }

  CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
  CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
  if (!hasAlpha) {
    // JPEG output is opaque; fill white so the corners of a rotated photo
    // do not turn black.
    CGContextFillRect(context, CGRectMake(0, 0, static_cast<CGFloat>(width),
                                          static_cast<CGFloat>(height)));
  }
  CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(width),
                                         static_cast<CGFloat>(height)),
                     image);

  // HEIC/JPEG decoders often hand back an image whose alphaInfo claims alpha
  // even when every pixel is opaque (phone photos are all like this). Emitting
  // a 4096 px PNG for those would balloon the PDF, so look at the pixels: only
  // keep the alpha channel when it is actually used.
  if (hasAlpha) {
    const uint8_t* pixels = static_cast<const uint8_t*>(CGBitmapContextGetData(context));
    bool nonOpaque = false;
    if (pixels != nullptr) {
      const size_t pixelCount = width * height;
      for (size_t i = 3; i < pixelCount * 4; i += 4) {
        if (pixels[i] != 255) {
          nonOpaque = true;
          break;
        }
      }
    }
    hasAlpha = nonOpaque;
  }

  CGImageRef scaled = CGBitmapContextCreateImage(context);
  CGContextRelease(context);
  return scaled;
}

bool transcodeWithImageIO(const uint8_t* data, size_t size,
                          MediaPrep::ImageContainer container,
                          const MediaPrep::TranscodeOptions& options,
                          MediaPrep::TranscodedImage& out, std::string& error) {
  @autoreleasepool {
    @try {
      if (data == nullptr || size == 0) {
        error = "Empty image payload";
        return false;
      }
      NSData* source = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(data)
                                            length:size
                                      freeWhenDone:NO];
      CGImageSourceRef imageSource = CGImageSourceCreateWithData((__bridge CFDataRef)source, nullptr);
      if (imageSource == nullptr) {
        error = std::string("ImageIO could not open the ") + MediaPrep::containerName(container) +
                " image";
        return false;
      }

      // Thumbnail with transform: ImageIO performs the progressive downscale
      // (cheap for HEIF/JPEG) and applies EXIF orientation in one pass.
      NSDictionary* thumbnailOptions = @{
        (__bridge NSString*)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (__bridge NSString*)kCGImageSourceCreateThumbnailWithTransform : @YES,
        (__bridge NSString*)kCGImageSourceShouldCacheImmediately : @YES,
        (__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize : @(options.maxPixelDimension),
      };
      CGImageRef image =
          CGImageSourceCreateThumbnailAtIndex(imageSource, 0, (__bridge CFDictionaryRef)
                                                                  thumbnailOptions);
      if (image == nullptr) {
        // Some exotic files refuse the thumbnail path; decode at full size and
        // let the SRGB render step scale it instead. CreateImageAtIndex does NOT
        // apply EXIF orientation (only the thumbnail API does), so bake it here
        // through CoreImage, which understands the EXIF orientation semantics.
        image = CGImageSourceCreateImageAtIndex(imageSource, 0, nullptr);
        if (image != nullptr) {
          NSDictionary* properties = (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(
              imageSource, 0, nullptr);
          NSNumber* orientationValue =
              properties[(__bridge NSString*)kCGImagePropertyOrientation];
          const int orientation = orientationValue.intValue;
          if (orientation > 1) {
            CIImage* ciImage = [CIImage imageWithCGImage:image];
            ciImage = [ciImage imageByApplyingOrientation:orientation];
            CIContext* ciContext = [CIContext contextWithOptions:@{
              kCIContextUseSoftwareRenderer : @YES
            }];
            CGImageRef oriented = [ciContext createCGImage:ciImage fromRect:ciImage.extent];
            if (oriented != nullptr) {
              CGImageRelease(image);
              image = oriented;
            }
          }
        }
      }
      CFRelease(imageSource);
      if (image == nullptr) {
        error = std::string("ImageIO failed to decode the ") + MediaPrep::containerName(container) +
                " image";
        return false;
      }

      bool hasAlpha = false;
      CGImageRef normalized = createScaledSRGBImage(image, options.maxPixelDimension, hasAlpha);
      CGImageRelease(image);
      if (normalized == nullptr) {
        error = "Failed to rasterize the image into an sRGB buffer";
        return false;
      }

      const size_t width = CGImageGetWidth(normalized);
      const size_t height = CGImageGetHeight(normalized);
      CFMutableDataRef encoded = CFDataCreateMutable(nullptr, 0);
      // PNG keeps the alpha channel for libharu's SMask; JPEG is smaller and
      // is what a photo page wants whenever the source is opaque.
      CFStringRef uti = hasAlpha ? CFSTR("public.png") : CFSTR("public.jpeg");
      CGImageDestinationRef destination =
          CGImageDestinationCreateWithData(encoded, uti, 1, nullptr);
      if (destination == nullptr) {
        CGImageRelease(normalized);
        CFRelease(encoded);
        error = "CGImageDestination could not be created";
        return false;
      }
      NSDictionary* properties =
          hasAlpha ? @{}
                   : @{
                       (__bridge NSString*)kCGImageDestinationLossyCompressionQuality :
                           @(static_cast<double>(options.jpegQuality) / 100.0),
                     };
      CGImageDestinationAddImage(destination, normalized, (__bridge CFDictionaryRef)properties);
      const bool finalized = CGImageDestinationFinalize(destination);
      CFRelease(destination);
      CGImageRelease(normalized);
      if (!finalized) {
        CFRelease(encoded);
        error = "Failed to encode the transcoded image";
        return false;
      }

      const CFIndex length = CFDataGetLength(encoded);
      const UInt8* bytes = CFDataGetBytePtr(encoded);
      out.bytes.assign(bytes, bytes + length);
      CFRelease(encoded);

      out.kind = hasAlpha ? MediaBlob::Kind::Png : MediaBlob::Kind::Jpeg;
      out.width = static_cast<uint32_t>(width);
      out.height = static_cast<uint32_t>(height);
      out.hadAlpha = hasAlpha;
      return true;
    } @catch (NSException* exception) {
      const char* reason = exception.reason != nil ? exception.reason.UTF8String : "(no reason)";
      error = std::string("ImageIO exception: ") + (reason != nullptr ? reason : "(no reason)");
      return false;
    }
  }
}

} // namespace

__attribute__((constructor)) static void NitroPdfWriterRegisterImageTranscoder(void) {
  MediaPrep::setPlatformImageTranscoder(&transcodeWithImageIO);
}

/**
 * Manual registration entry point. Callers: the ObjC `+load` below, the C++
 * constructor, and `transcodeToPdfImage` itself (through a weak symbol) so a
 * linker that drops the archive member cannot silently disable HEIC/WebP.
 * Idempotent.
 */
extern "C" void NitroPdfWriterEnsurePlatformImageTranscoder(void) {
  MediaPrep::setPlatformImageTranscoder(&transcodeWithImageIO);
}

/**
 * ObjC class so `-ObjC` pulls this object file out of a static archive. The
 * plain `__attribute__((constructor))` above is invisible to the linker's
 * archive-member selection: with `static_framework = true` and no symbol
 * referenced from outside, the whole translation unit can be dropped and every
 * HEIC/WebP load would fail with "platform decoder not registered".
 */
@interface NitroPdfWriterImageTranscoderRegistration : NSObject
@end

@implementation NitroPdfWriterImageTranscoderRegistration
+ (void)load {
  NitroPdfWriterEnsurePlatformImageTranscoder();
}
@end
