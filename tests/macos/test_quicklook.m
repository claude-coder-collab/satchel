// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Builds a zip through the C API, then renders it with the Quick Look provider class.
#import <Foundation/Foundation.h>

#include "zp/zp.h"

@interface PreviewProvider : NSObject
+ (NSString*)htmlForURL:(NSURL*)url error:(NSError**)error;
@end

static int failures = 0;

static void check(BOOL ok, const char* what)
{
    if (!ok)
    {
        fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    @autoreleasepool
    {
        NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* zip = [dir stringByAppendingPathComponent:@"q & a.zip"];

        zp_context_t* ctx = zp_context_create(2, 0);
        zp_input_t* input = zp_input_memory();
        const char text[] = "hello quick look";
        zp_input_memory_add_file(input, "docs/<hello>.txt", (const uint8_t*)text, sizeof text - 1, 1700000000, 0644);
        zp_plan_t* plan = zp_plan_create(ctx, input, NULL);
        zp_stream_t* out = zp_stream_create_file(zip.fileSystemRepresentation);
        check(plan && out && zp_build(plan, out, NULL, NULL, NULL, NULL) == ZP_OK && zp_stream_commit(out) == ZP_OK, "build");
        zp_stream_free(out);
        zp_plan_free(plan);
        zp_input_free(input);
        zp_context_free(ctx);

        NSError* error = nil;
        NSString* html = [PreviewProvider htmlForURL:[NSURL fileURLWithPath:zip] error:&error];
        check(html != nil, "zip preview");
        check([html containsString:@"<h1>q &amp; a.zip</h1>"], "escaped title");
        check([html containsString:@"&lt;hello&gt;.txt"], "escaped entry");

        NSString* other = [dir stringByAppendingPathComponent:@"plain.txt"];
        [@"not a zip" writeToFile:other atomically:YES encoding:NSUTF8StringEncoding error:nil];
        error = nil;
        check([PreviewProvider htmlForURL:[NSURL fileURLWithPath:other] error:&error] == nil && error != nil, "other files fail with an error");
        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    }
    if (failures == 0)
        printf("quick look provider ok\n");
    return failures == 0 ? 0 : 1;
}
