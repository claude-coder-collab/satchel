// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#import <Foundation/Foundation.h>
#import <QuickLookUI/QuickLookUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "zp/zp.h"

// Quick Look preview of zip archives and of FLAC files made by Satchel, rendered by the core's
// zp_preview (metadata only, never audio).
@interface PreviewProvider : QLPreviewProvider <QLPreviewingController>
@end

@implementation PreviewProvider

+ (NSString*)htmlForURL:(NSURL*)url error:(NSError**)error
{
    zp_stream_t* stream = zp_stream_open_file(url.fileSystemRepresentation);
    char* text = stream ? zp_preview(stream, url.lastPathComponent.UTF8String, ZP_PREVIEW_HTML) : NULL;
    NSString* html = text ? [NSString stringWithUTF8String:text] : nil;
    if (!html && error)
        *error = [NSError errorWithDomain:@"Satchel" code:zp_last_status() userInfo:@{ NSLocalizedDescriptionKey : [NSString stringWithUTF8String:zp_last_error()] }];
    zp_free(text);
    zp_stream_free(stream);
    return html;
}

- (void)providePreviewForFileRequest:(QLFilePreviewRequest*)request completionHandler:(void (^)(QLPreviewReply* _Nullable, NSError* _Nullable))handler
{
    NSError* error = nil;
    NSString* html = [PreviewProvider htmlForURL:request.fileURL error:&error];
    if (!html)
    {
        handler(nil, error);
        return;
    }
    QLPreviewReply* reply = [[QLPreviewReply alloc] initWithDataOfContentType:UTTypeHTML
                                                                 contentSize:CGSizeMake(760, 560)
                                                           dataCreationBlock:^NSData* _Nullable(QLPreviewReply* r, NSError** e) {
                                                             (void)e;
                                                             r.stringEncoding = NSUTF8StringEncoding;
                                                             return [html dataUsingEncoding:NSUTF8StringEncoding];
                                                           }];
    reply.title = request.fileURL.lastPathComponent;
    handler(reply, nil);
}

@end
