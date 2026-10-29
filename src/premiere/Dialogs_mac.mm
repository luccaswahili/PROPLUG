// macOS file dialogs for "Load Reference..." and "Export .cube...".
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cstring>
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
        } else {  // macOS 10.15
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
