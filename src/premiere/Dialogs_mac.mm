// macOS file dialogs for "Load Reference..." and "Export .cube...".
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cstring>
#include <unistd.h>
#include "LookMatch.h"

static bool copyPath(NSURL* url, char* out, int capacity) {
    const char* s = url.fileSystemRepresentation;
    if (!s || int(std::strlen(s)) >= capacity) return false;
    std::strcpy(out, s);
    return true;
}

bool LM_OpenImageDialog(char* pathOut, int capacity) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"LookMatch: choose the reference still";
        if (@available(macOS 11.0, *)) {
            panel.allowedContentTypes = @[ UTTypePNG, UTTypeJPEG, UTTypeBMP, [UTType typeWithFilenameExtension:@"tga"] ];
        } else {
            // macOS 10.9-10.15 (Premiere CC 2015-2020 era): no UniformTypeIdentifiers framework
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            panel.allowedFileTypes = @[ @"png", @"jpg", @"jpeg", @"bmp", @"tga" ];
#pragma clang diagnostic pop
        }
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK) return false;
        return copyPath(panel.URLs.firstObject, pathOut, capacity);
    }
}

bool LM_SaveCubeDialog(char* pathOut, int capacity) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = @"LookMatch: export LUT for Lumetri";
        panel.nameFieldStringValue = @"LookMatch.cube";
        if ([panel runModal] != NSModalResponseOK) return false;
        return copyPath(panel.URL, pathOut, capacity);
    }
}

// "Open Komodo's Panel" in Effect Controls. A native effect has no API to show a UXP panel, so this
// picks Window > UXP Plugins > Komodo from Premiere's own menu bar through System Events. It runs in
// osascript off the main thread: a synchronous click from inside the effect call would wait on the
// very main thread it is blocking. macOS asks once to let Premiere use Accessibility; until it is
// allowed (or if the menu isn't there) the user gets a note saying where the panel lives.
// The menu item is a toggle (ticked while the panel is open), so an open panel is closed and reopened,
// which brings it to the front even when it sits in a tab behind others.
void LM_OpenKomodoPanel() {
    NSString* script = [NSString stringWithFormat:
        @"tell application \"System Events\"\n"
         "  set p to first application process whose unix id is %d\n"
         "  set frontmost of p to true\n"
         "  tell p\n"
         "    set k to menu item \"Komodo\" of menu 1 of menu item \"UXP Plugins\" of menu 1 of menu bar item \"Window\" of menu bar 1\n"
         "    set shown to false\n"
         "    try\n"
         "      set shown to (value of attribute \"AXMenuItemMarkChar\" of k) is not missing value\n"
         "    end try\n"
         "    click k\n"
         "    if shown then\n"
         "      delay 0.3\n"
         "      click k\n"
         "    end if\n"
         "  end tell\n"
         "end tell\n",
        int(getpid())];
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
        @autoreleasepool {
            NSTask* task = [[NSTask alloc] init];
            task.launchPath = @"/usr/bin/osascript";
            task.arguments = @[ @"-e", script ];
            task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
            task.standardError = [NSFileHandle fileHandleWithNullDevice];
            int status = -1;
            @try {
                [task launch];
                [task waitUntilExit];
                status = task.terminationStatus;
            } @catch (NSException*) {
            }
            [task release];  // this file is built without ARC
            if (status == 0) return;
            dispatch_async(dispatch_get_main_queue(), ^{
                NSAlert* alert = [[NSAlert alloc] init];
                alert.messageText = @"Open Komodo from Window > UXP Plugins > Komodo";
                alert.informativeText =
                    @"To let this button open it for you, allow Adobe Premiere Pro in System Settings > "
                     "Privacy & Security > Accessibility, then click Open Komodo's Panel again.";
                [alert addButtonWithTitle:@"OK"];
                [alert runModal];
                [alert release];
            });
        }
    });
}
