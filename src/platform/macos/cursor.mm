/**
 * @file src/platform/macos/cursor.mm
 * @brief Definitions for reading the macOS pointer for clients that draw it locally.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <dlfcn.h>

// platform includes
#import <AppKit/AppKit.h>

// local includes
#include "src/cursor_sync.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {
  namespace {
    /**
     * @brief Pointer of the macOS session, read from WindowServer and AppKit.
     */
    class macos_cursor_t final: public cursor_sync::backend_t {
    public:
      macos_cursor_t():
          // Private but long-standing WindowServer call that changes whenever any app sets a pointer;
          // without it the pointer would have to be rendered and compared on every check.
          seed_fn(reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "CGSCurrentCursorSeed"))),
          // Deprecated without a replacement, but it still reports pointers hidden by any app.
          visible_fn(reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "CGCursorIsVisible"))) {
        if (!seed_fn) {
          BOOST_LOG(warning) << "Pointer sync: CGSCurrentCursorSeed is unavailable, so pointer changes are only seen with visibility changes"sv;
        }
      }

      std::uint64_t image_seed() override {
        return seed_fn ? static_cast<std::uint32_t>(seed_fn()) : 0;
      }

      bool visible() override {
        return !visible_fn || visible_fn() != 0;
      }

      std::optional<cursor_sync::image_t> image(int stream_width) override {
        @autoreleasepool {
          NSCursor *cursor = [NSCursor currentSystemCursor];
          NSImage *image = cursor.image;
          const NSSize size = image.size;
          if (!image || size.width <= 0 || size.height <= 0) {
            return std::nullopt;
          }

          const double scale = points_to_stream_pixels(stream_width);
          const int width = std::max(1, static_cast<int>(std::lround(size.width * scale)));
          const int height = std::max(1, static_cast<int>(std::lround(size.height * scale)));

          // Picks the sharpest representation for the size, usually the Retina one.
          NSRect rect = NSMakeRect(0, 0, width, height);
          CGImageRef source = [image CGImageForProposedRect:&rect context:nil hints:nil];
          if (!source) {
            return std::nullopt;
          }

          CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
          CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, color_space, kCGImageAlphaPremultipliedLast);
          CGColorSpaceRelease(color_space);
          if (!context) {
            return std::nullopt;
          }
          CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
          CGContextDrawImage(context, CGRectMake(0, 0, width, height), source);
          CGImageRef scaled = CGBitmapContextCreateImage(context);
          CGContextRelease(context);
          if (!scaled) {
            return std::nullopt;
          }

          NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:scaled];
          CGImageRelease(scaled);
          NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@ {}];
          [rep release];
          if (!png) {
            return std::nullopt;
          }

          // The hotspot is in points from the top-left corner of the image.
          const NSPoint hotspot = cursor.hotSpot;
          return cursor_sync::image_t {
            std::string {static_cast<const char *>(png.bytes), png.length},
            std::clamp(static_cast<int>(std::lround(hotspot.x * scale)), 0, width - 1),
            std::clamp(static_cast<int>(std::lround(hotspot.y * scale)), 0, height - 1),
          };
        }
      }

    private:
      /**
       * @brief Stream pixels per point on the display under the pointer.
       *
       * @param stream_width Width of the client's video stream.
       * @return Scale from pointer points to stream pixels.
       */
      static double points_to_stream_pixels(int stream_width) {
        CGEventRef event = CGEventCreate(nullptr);
        const CGPoint location = event ? CGEventGetLocation(event) : CGPointZero;
        if (event) {
          CFRelease(event);
        }

        CGDirectDisplayID display = CGMainDisplayID();
        std::uint32_t count = 0;
        CGGetDisplaysWithPoint(location, 1, &display, &count);
        const double display_width = CGDisplayBounds(display).size.width;
        return stream_width > 0 && display_width > 0 ? stream_width / display_width : 2.0;
      }

      int (*seed_fn)();  ///< CGSCurrentCursorSeed, if present.
      int (*visible_fn)();  ///< CGCursorIsVisible, if present.
    };
  }  // namespace

  std::unique_ptr<cursor_sync::backend_t> make_cursor() {
    return std::make_unique<macos_cursor_t>();
  }
}  // namespace platf
