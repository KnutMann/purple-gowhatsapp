/* Marks a downloaded file with the system's quarantine attribute.
 *
 * The Go side writes attachments with a plain os.Create, which leaves no
 * com.apple.quarantine on the file - so when the user later double-clicks a
 * received document in Finder, Gatekeeper never interposes, unlike a browser
 * download. This stamps the file through the supported Foundation API rather
 * than hand-writing the xattr, so the format stays whatever the system says
 * it is; the system fills in the timestamp and identifier itself.
 *
 * Called from plain C, kept free of ARC-or-not assumptions: nothing here
 * outlives the autorelease pool.
 */

#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>

#include "gowhatsapp.h"

void gowhatsapp_quarantine_downloaded_file(const char *local_path)
{
    if (local_path == NULL || local_path[0] == 0) {
        return;
    }
    @autoreleasepool {
        NSString *path = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:local_path
                                                                                     length:strlen(local_path)];
        if (path == nil) {
            return;
        }
        NSURL *url = [NSURL fileURLWithPath:path];
        NSDictionary *properties = @{
            (__bridge NSString *)kLSQuarantineAgentNameKey: @"Adium",
            (__bridge NSString *)kLSQuarantineTypeKey: (__bridge NSString *)kLSQuarantineTypeOtherDownload,
        };
        NSError *error = nil;
        if (![url setResourceValue:properties forKey:NSURLQuarantinePropertiesKey error:&error]) {
            purple_debug_warning(GOWHATSAPP_NAME, "Could not quarantine %s: %s\n",
                                 local_path, error.localizedDescription.UTF8String ?: "unknown");
        }
    }
}
