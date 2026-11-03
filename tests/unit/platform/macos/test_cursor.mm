/**
 * @file tests/unit/platform/macos/test_cursor.mm
 * @brief Unit tests for src/platform/macos/cursor.mm.
 */
// Only compile these tests on macOS
#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include <src/cursor_sync.h>

TEST(MacosCursorTest, ReadsThePointerWithoutFailing) {
  auto cursor = platf::make_cursor();
  ASSERT_NE(cursor, nullptr);

  cursor->image_seed();
  cursor->visible();

  // Without a window server session, as over SSH, there may be no pointer to render.
  if (const auto image = cursor->image(2560)) {
    EXPECT_FALSE(image->png.empty());
    EXPECT_GE(image->hotspot_x, 0);
    EXPECT_GE(image->hotspot_y, 0);
  }
}
#endif
