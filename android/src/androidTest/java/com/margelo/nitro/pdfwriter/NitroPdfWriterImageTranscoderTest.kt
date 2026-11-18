package com.margelo.nitro.pdfwriter

import android.graphics.BitmapFactory
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device coverage for the HEIC/WebP transcode: real ImageDecoder /
 * BitmapFactory, real fixtures, the exact bytes libharu will receive.
 *
 * Run with: ./gradlew :react-native-nitro-pdf-writer:connectedDebugAndroidTest
 */
@RunWith(AndroidJUnit4::class)
class NitroPdfWriterImageTranscoderTest {

  private fun assetBytes(name: String): ByteArray {
    val context = InstrumentationRegistry.getInstrumentation().context
    return context.assets.open(name).use { input ->
      val out = ByteArrayOutputStream()
      val buffer = ByteArray(8192)
      while (true) {
        val read = input.read(buffer)
        if (read <= 0) break
        out.write(buffer, 0, read)
      }
      out.toByteArray()
    }
  }

  private fun isJpeg(bytes: ByteArray): Boolean =
    bytes.size >= 3 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte() && bytes[2] == 0xFF.toByte()

  private fun isPng(bytes: ByteArray): Boolean =
    bytes.size >= 8 && bytes[0] == 0x89.toByte() && bytes[1] == 'P'.code.toByte() &&
      bytes[2] == 'N'.code.toByte() && bytes[3] == 'G'.code.toByte()

  private fun decodeSize(bytes: ByteArray): Pair<Int, Int> {
    val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
    assertTrue("decoded payload is not a readable image", options.outWidth > 0)
    return options.outWidth to options.outHeight
  }

  private fun apiLevel(): Int = android.os.Build.VERSION.SDK_INT

  private fun requireHeifSupport(): Boolean = apiLevel() >= ImageTranscodePolicy.HEIF_MIN_API

  @Test
  fun heicOpaqueEncodesAsJpeg() {
    assumeTrue("HEIF decoding needs API 28+", requireHeifSupport())
    val source = assetBytes("heic_opaque_16x16.heic")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/heic", 4096, 85)

    assertTrue("opaque HEIC must become JPEG, got ${encoded.take(4)}", isJpeg(encoded))
    assertEquals(16 to 16, decodeSize(encoded))
  }

  @Test
  fun heicWithTransparencyDecodesToAValidImage() {
    assumeTrue("HEIF decoding needs API 28+", requireHeifSupport())
    val source = assetBytes("heic_alpha_64x64.heic")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/heic", 4096, 85)

    // Android's platform HEIF decoder drops the auxiliary alpha image (verified
    // on API 36: BitmapFactory and ImageDecoder both report hasAlpha() == false),
    // so this device lands on opaque JPEG. iPhones that keep alpha go through
    // the PNG branch instead; accept either but never a broken payload.
    assertTrue("encoded payload must be PNG or JPEG", isPng(encoded) || isJpeg(encoded))
    assertEquals(64 to 64, decodeSize(encoded))
    if (isPng(encoded)) {
      val bitmap = BitmapFactory.decodeByteArray(encoded, 0, encoded.size)
      assertNotNull(bitmap)
      assertTrue("PNG output must carry alpha", bitmap.hasAlpha())
    }
  }


  @Test
  fun webpOpaqueEncodesAsJpeg() {
    val source = assetBytes("webp_opaque_16x16.webp")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/webp", 4096, 85)

    assertTrue("opaque WebP must become JPEG", isJpeg(encoded))
    assertEquals(16 to 16, decodeSize(encoded))
  }

  @Test
  fun webpWithTransparencyEncodesAsPng() {
    val source = assetBytes("webp_alpha_64x64.webp")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/webp", 4096, 85)

    assertTrue("transparent WebP must keep alpha as PNG", isPng(encoded))
    assertEquals(64 to 64, decodeSize(encoded))
    val bitmap = BitmapFactory.decodeByteArray(encoded, 0, encoded.size)
    assertNotNull(bitmap)
    assertTrue(bitmap.hasAlpha())
  }

  @Test
  fun largeHeicIsDownscaledToThePixelBound() {
    assumeTrue("HEIF decoding needs API 28+", requireHeifSupport())
    // 4032x3024 = a typical 12MP phone photo.
    val source = assetBytes("heic_12mp_4032x3024.heic")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/heic", 2048, 85)

    assertEquals("long edge must be clamped to 2048", 2048 to 1536, decodeSize(encoded))
  }

  @Test
  fun customPixelBoundIsRespected() {
    assumeTrue("HEIF decoding needs API 28+", requireHeifSupport())
    val source = assetBytes("heic_12mp_4032x3024.heic")
    val encoded = NitroPdfWriterImageTranscoder.decode(source, "image/heic", 1024, 60)

    val size = decodeSize(encoded)
    assertEquals(1024, size.first)
    assertEquals(768, size.second) // 3024 * 1024 / 4032 = 768
  }

  @Test
  fun heicIsRefusedWithAReadableMessageBelowApi28() {
    // The device cannot be downgraded, so assert the policy gate directly: this
    // is the exact message the Kotlin bridge throws and JNI forwards to JS.
    val message = ImageTranscodePolicy.unsupportedMessage("image/heic", 26)
    assertNotNull(message)
    assertTrue(message!!.contains("API 28"))
    assertTrue(message.contains("API 26"))
  }

  @Test
  fun corruptPayloadFailsWithAMessage() {
    // Below API 28 the policy gate fires first ("requires Android 9"), so the
    // decoder-level message this test asserts would never be produced.
    assumeTrue("decoder-level failure needs a working HEIF path", requireHeifSupport())
    val garbage = byteArrayOf(1, 2, 3, 4, 5)
    val error = try {
      NitroPdfWriterImageTranscoder.decode(garbage, "image/heic", 4096, 85)
      null
    } catch (exception: IllegalStateException) {
      exception
    }
    assertNotNull("decoding garbage must throw", error)
    assertTrue(error!!.message!!.contains("image/heic"))
  }
}
