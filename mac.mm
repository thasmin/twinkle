#include "mac.h"
#include <iostream>
#import <Cocoa/Cocoa.h>

extern void file_callback(const std::string& path);

std::string path() {
	NSOpenGLContext *foo = [NSOpenGLContext currentContext];

    NSOpenPanel* panel = [NSOpenPanel openPanel];
	NSArray* videoTypes = @[@"mp4", @"mov"];
	[panel setAllowedFileTypes:videoTypes];

	int response = [panel runModal];
	[foo makeCurrentContext];
    if (response == NSModalResponseOK) {
		NSURL *nsurl = [[panel URLs] objectAtIndex:0];
		return std::string([[nsurl path] UTF8String]);
	}
	return "";
}
