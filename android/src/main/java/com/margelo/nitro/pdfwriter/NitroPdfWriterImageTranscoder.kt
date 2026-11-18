package com.margelo.nitro.pdfwriter

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.media.ExifInterface
import android.os.Build
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/**
 * Platform decoder for image containers that PDF cannot embed (HEIC/HEIF,
 * WebP, AVIF).
 *
 * The C++ core recognises these by magic bytes and calls [decode] through JNI;
 * the returned bytes are an encoded PNG (when the source carries alpha) or JPEG
 * that libharu embeds directly. Failures are reported as exceptions whose
 * message is surfaced to JS as a rejected promise, which is how HEIC on
 * Android 7/8 (API 24-27, no platform HEIF decoder) is refused.
 */
object NitroPdfWriterImageTranscoder {
  init {
    NitroPdfWriterOnLoad.initializeNative()
    register()
  }

  /**
   * Registers the native decode callback. Called from the object initializer;
   * the implementation lives in android/AndroidImageTranscoder.cpp.
   */
  @JvmStatic
  private external fun register()

  /** Touch this object from NitroPdfWriterPackage so the hook is installed early. */
  @JvmStatic
  fun setup() {
    // Referencing the object runs the init block above.
  }

  /**
   * @param bytes             encoded HEIC/WebP/AVIF payload
   * @param mime              `image/heic`, `image/webp` or `image/avif`
   * @param maxPixelDimension long-edge bound applied when scaling down
   * @param jpegQuality       1-100, used only when the output is opaque JPEG
   * @return encoded PNG (source had alpha) or JPEG bytes
   */
  @JvmStatic
  fun decode(bytes: ByteArray, mime: String, maxPixelDimension: Int, jpegQuality: Int): ByteArray {
    val bitmap = decodeBitmap(bytes, mime, maxPixelDimension)
    try {
      val out = ByteArrayOutputStream()
      val hasAlpha = bitmap.hasAlpha()
      val png = ImageTranscodePolicy.isPng(hasAlpha)
      val format = if (png) Bitmap.CompressFormat.PNG else Bitmap.CompressFormat.JPEG
      val quality = if (png) 100 else ImageTranscodePolicy.clampQuality(jpegQuality)
      if (!bitmap.compress(format, quality, out)) {
        throw IllegalStateException("Bitmap.compress failed for $mime")
      }
      return out.toByteArray()
    } finally {
      bitmap.recycle()
    }
  }

  private fun decodeBitmap(bytes: ByteArray, mime: String, maxPixelDimension: Int): Bitmap {
    ImageTranscodePolicy.unsupportedMessage(mime, Build.VERSION.SDK_INT)?.let {
      throw IllegalStateException(it)
    }

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      // ImageDecoder applies EXIF orientation itself (AOSP swaps width/height
      // for the 90-degree cases), so no manual rotation is needed here.
      try {
        val source = ImageDecoder.createSource(ByteBuffer.wrap(bytes))
        val bitmap = ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
          decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
          // Bounding during decode instead of after keeps large HEICs (24MP+)
          // from allocating a full-size bitmap first; some decoders refuse to
          // hand back pixels that big.
          ImageTranscodePolicy.scaledSize(
            info.size.width, info.size.height, maxPixelDimension
          )?.let { decoder.setTargetSize(it.width, it.height) }
        }
        return scaleDown(bitmap, maxPixelDimension)
      } catch (error: Exception) {
        // Fall through: a few vendor HEIF decoders reject payloads through
        // ImageDecoder while BitmapFactory's bundled path still copes, and
        // vice versa. Try the other entry point before giving up.
        try {
          return decodeWithBitmapFactory(bytes, mime, maxPixelDimension)
        } catch (_: Exception) {
          throw IllegalStateException(
            "Failed to decode $mime image: ${error.message}", error)
        }
      }
    }

    // API 24-27: HEIF/AVIF were already rejected above, so this only handles WebP.
    return decodeWithBitmapFactory(bytes, mime, maxPixelDimension)
  }

  private fun decodeWithBitmapFactory(
    bytes: ByteArray,
    mime: String,
    maxPixelDimension: Int
  ): Bitmap {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
      throw IllegalArgumentException("Cannot decode $mime image")
    }
    val options = BitmapFactory.Options().apply {
      inSampleSize = ImageTranscodePolicy.sampleSize(
        bounds.outWidth, bounds.outHeight, maxPixelDimension)
      inPreferredConfig = Bitmap.Config.ARGB_8888
    }
    val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
      ?: throw IllegalArgumentException("Cannot decode $mime image")
    // BitmapFactory ignores EXIF orientation; WebP rarely carries it, but JPEG
    // payloads mislabelled as WebP would otherwise land sideways.
    val rotated = applyExifOrientation(bytes, bitmap)
    return scaleDown(rotated, maxPixelDimension)
  }

  private fun scaleDown(bitmap: Bitmap, maxPixelDimension: Int): Bitmap {
    val target = ImageTranscodePolicy.scaledSize(
      bitmap.width, bitmap.height, maxPixelDimension) ?: return bitmap
    val scaled = Bitmap.createScaledBitmap(bitmap, target.width, target.height, true)
    if (scaled != bitmap) {
      bitmap.recycle()
    }
    return scaled
  }

  private fun applyExifOrientation(bytes: ByteArray, bitmap: Bitmap): Bitmap {
    val orientation = try {
      ByteArrayInputStream(bytes).use {
        ExifInterface(it).getAttributeInt(
          ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
      }
    } catch (_: Exception) {
      ExifInterface.ORIENTATION_NORMAL
    }
    val matrix = android.graphics.Matrix()
    when (orientation) {
      ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
      ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
      ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(270f)
      ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
      ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
      ExifInterface.ORIENTATION_TRANSPOSE -> {
        matrix.postRotate(90f)
        matrix.postScale(-1f, 1f)
      }
      ExifInterface.ORIENTATION_TRANSVERSE -> {
        matrix.postRotate(270f)
        matrix.postScale(-1f, 1f)
      }
      else -> return bitmap
    }
    val rotated = Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
    if (rotated != bitmap) {
      bitmap.recycle()
    }
    return rotated
  }
}
