/**
 * @file src/platform/macos/clipboard.mm
 * @brief Definitions for the macOS clipboard used by client clipboard sync.
 */
// platform includes
#import <AppKit/AppKit.h>

// local includes
#include "src/clipboard_sync.h"

namespace platf {
  namespace {
    /**
     * @brief Check whether the pasteboard holds data its owner asked others not to keep.
     *
     * Password managers mark copied secrets with the nspasteboard.org concealed and transient types.
     *
     * @param pasteboard Pasteboard to inspect.
     * @return `true` when the contents must not leave this Mac.
     */
    bool is_private(NSPasteboard *pasteboard) {
      for (NSPasteboardType type in pasteboard.types) {
        if ([type isEqualToString:@"org.nspasteboard.ConcealedType"] || [type isEqualToString:@"org.nspasteboard.TransientType"]) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Copy NSData into a byte string.
     *
     * @param data Data to copy.
     * @return Bytes of `data`.
     */
    std::string to_bytes(NSData *data) {
      return std::string {static_cast<const char *>(data.bytes), data.length};
    }

    /**
     * @brief Clipboard backed by the general NSPasteboard.
     */
    class macos_clipboard_t final: public clipboard_sync::backend_t {
    public:
      std::int64_t sequence() override {
        @autoreleasepool {
          return [NSPasteboard generalPasteboard].changeCount;
        }
      }

      std::optional<clipboard_sync::item_t> read() override {
        @autoreleasepool {
          NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
          if (is_private(pasteboard)) {
            return std::nullopt;
          }

          // Text wins: copying from Office, browsers and most editors also puts a rendered image on the pasteboard.
          if (NSString *text = [pasteboard stringForType:NSPasteboardTypeString]; text.length > 0) {
            return clipboard_sync::item_t {std::string {clipboard_sync::text_mime_type}, std::string {text.UTF8String}};
          }

          NSData *png = [pasteboard dataForType:NSPasteboardTypePNG];
          if (!png) {
            if (NSData *tiff = [pasteboard dataForType:NSPasteboardTypeTIFF]) {
              png = [[NSBitmapImageRep imageRepWithData:tiff] representationUsingType:NSBitmapImageFileTypePNG properties:@ {}];
            }
          }
          if (png) {
            return clipboard_sync::item_t {std::string {clipboard_sync::png_mime_type}, to_bytes(png)};
          }
          return std::nullopt;
        }
      }

      std::vector<std::filesystem::path> files() override {
        @autoreleasepool {
          std::vector<std::filesystem::path> paths;
          NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
          if (is_private(pasteboard)) {
            return paths;
          }

          NSArray<NSURL *> *urls = [pasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
          for (NSURL *url in urls) {
            if (const char *path = url.fileSystemRepresentation) {
              paths.emplace_back(path);
            }
          }
          return paths;
        }
      }

      bool write_files(const std::vector<std::filesystem::path> &paths) override {
        @autoreleasepool {
          NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:paths.size()];
          for (const auto &path : paths) {
            [urls addObject:[NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:NO relativeToURL:nil]];
          }

          NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
          [pasteboard clearContents];
          return [pasteboard writeObjects:urls];
        }
      }

      bool write(const clipboard_sync::item_t &item) override {
        @autoreleasepool {
          NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
          if (item.mime_type == clipboard_sync::text_mime_type) {
            NSString *text = [[[NSString alloc] initWithBytes:item.data.data() length:item.data.size() encoding:NSUTF8StringEncoding] autorelease];
            if (!text) {
              return false;
            }
            [pasteboard clearContents];
            return [pasteboard setString:text forType:NSPasteboardTypeString];
          }

          if (item.mime_type == clipboard_sync::png_mime_type) {
            NSData *png = [NSData dataWithBytes:item.data.data() length:item.data.size()];
            NSImage *image = [[[NSImage alloc] initWithData:png] autorelease];
            if (!image) {
              return false;
            }
            [pasteboard clearContents];
            // NSImage adds TIFF for apps that do not read PNG.
            return [pasteboard writeObjects:@[image]];
          }
          return false;
        }
      }
    };
  }  // namespace

  std::unique_ptr<clipboard_sync::backend_t> make_clipboard() {
    return std::make_unique<macos_clipboard_t>();
  }

  std::filesystem::path clipboard_transfer_directory() {
    @autoreleasepool {
      NSURL *downloads = [NSFileManager.defaultManager URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
      if (!downloads) {
        return {};
      }
      return std::filesystem::path {downloads.fileSystemRepresentation} / "Moonlight";
    }
  }
}  // namespace platf
