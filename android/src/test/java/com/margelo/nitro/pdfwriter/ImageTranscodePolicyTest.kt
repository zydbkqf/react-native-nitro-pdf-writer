package com.margelo.nitro.pdfwriter

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM unit tests for the pure transcode decisions (no android.graphics calls).
 * The bitmap work itself is covered on-device by
 * android/src/androidTest/.../NitroPdfWriterImageTranscoderTest.kt.
 */
class ImageTranscodePolicyTest {

  @Test
  fun `heic is rejected below API 28`() {
    val message = ImageTranscodePolicy.unsupportedMessage("image/heic", 24)
    assertNotNull(message)
    assertTrue(message!!.contains("API 28"))
    assertTrue(message.contains("API 24"))
  }

  @Test
  fun `heic is allowed from API 28`() {
    assertNull(ImageTranscodePolicy.unsupportedMessage("image/heic", 28))
    assertNull(ImageTranscodePolicy.unsupportedMessage("image/heif", 36))
  }

  @Test
  fun `avif is rejected below API 31`() {
    val message = ImageTranscodePolicy.unsupportedMessage("image/avif", 30)
    assertNotNull(message)
    assertTrue(message!!.contains("API 31"))
    assertNull(ImageTranscodePolicy.unsupportedMessage("image/avif", 31))
  }

  @Test
  fun `webp is never gated by API level`() {
    assertNull(ImageTranscodePolicy.unsupportedMessage("image/webp", 24))
    assertNull(ImageTranscodePolicy.unsupportedMessage("image/webp", 16))
  }

  @Test
  fun `unknown mime is not gated`() {
    assertNull(ImageTranscodePolicy.unsupportedMessage("application/octet-stream", 24))
  }

  @Test
  fun `sampleSize keeps the long edge at or above the target`() {
    // Already small enough: no subsampling.
    assertEquals(1, ImageTranscodePolicy.sampleSize(6000, 4000, 4096))
    assertEquals(1, ImageTranscodePolicy.sampleSize(16, 16, 4096))
    // 9000 / 2 = 4500 >= 4096, 9000 / 4 = 2250 < 4096 -> 2.
    assertEquals(2, ImageTranscodePolicy.sampleSize(9000, 6000, 4096))
    // Powers of two only.
    assertEquals(4, ImageTranscodePolicy.sampleSize(16384, 8192, 4096))
    // A zero/negative bound means "no limiting".
    assertEquals(1, ImageTranscodePolicy.sampleSize(9000, 6000, 0))
  }

  @Test
  fun `scaledSize preserves aspect ratio and bounds the long edge`() {
    val target = ImageTranscodePolicy.scaledSize(6000, 4000, 4096)
    assertNotNull(target)
    assertEquals(4096, target!!.width)
    // 4000 * 4096 / 6000 = 2730.66 -> 2730
    assertEquals(2730, target.height)

    val portrait = ImageTranscodePolicy.scaledSize(3000, 6000, 4096)
    assertEquals(2048, portrait!!.width)
    assertEquals(4096, portrait.height)

    // Fits already: no scaling needed.
    assertNull(ImageTranscodePolicy.scaledSize(16, 16, 4096))
    assertNull(ImageTranscodePolicy.scaledSize(4096, 2048, 4096))
  }

  @Test
  fun `scaledSize never produces a zero dimension`() {
    val target = ImageTranscodePolicy.scaledSize(100000, 1, 16)
    assertNotNull(target)
    assertEquals(16, target!!.width)
    assertEquals(1, target.height)
  }

  @Test
  fun `format selection follows alpha`() {
    assertTrue(ImageTranscodePolicy.isPng(true))
    assertTrue(!ImageTranscodePolicy.isPng(false))
  }

  @Test
  fun `jpeg quality is clamped`() {
    assertEquals(1, ImageTranscodePolicy.clampQuality(0))
    assertEquals(100, ImageTranscodePolicy.clampQuality(200))
    assertEquals(85, ImageTranscodePolicy.clampQuality(85))
  }
}
