// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "mac_services.hpp"

#include "app.hpp"

#include <QMetaObject>

#import <AppKit/AppKit.h>

@interface SatchelServices : NSObject
- (instancetype)initWithApp:(App*)app;
- (void)compressFiles:(NSPasteboard*)pboard userData:(NSString*)userData error:(NSString**)error;
- (void)extractFiles:(NSPasteboard*)pboard userData:(NSString*)userData error:(NSString**)error;
@end

@implementation SatchelServices
{
    App* app_;
}

- (instancetype)initWithApp:(App*)app
{
    self = [super init];
    if (self)
        app_ = app;
    return self;
}

- (void)forward:(NSPasteboard*)pboard intent:(satchel_gui::Intent)intent error:(NSString**)error
{
    NSArray<NSURL*>* urls = [pboard readObjectsForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    QStringList paths;
    for (NSURL* url in urls)
        paths << QString::fromNSString(url.path);
    if (paths.isEmpty())
    {
        if (error)
            *error = @"No files were passed to Satchel.";
        return;
    }
    App* app = app_;
    QMetaObject::invokeMethod(app, [app, paths, intent]
                              { app->open_paths(paths, intent); }, Qt::QueuedConnection);
}

- (void)compressFiles:(NSPasteboard*)pboard userData:(NSString*)userData error:(NSString**)error
{
    (void)userData;
    [self forward:pboard intent:satchel_gui::Intent::Compress error:error];
}

- (void)extractFiles:(NSPasteboard*)pboard userData:(NSString*)userData error:(NSString**)error
{
    (void)userData;
    [self forward:pboard intent:satchel_gui::Intent::Extract error:error];
}
@end

namespace
{

SatchelServices* provider_for(App& app)
{
    static SatchelServices* provider = nil;
    if (!provider)
        provider = [[SatchelServices alloc] initWithApp:&app];
    return provider;
}

} // namespace

void install_mac_services(App& app)
{
    [NSApp setServicesProvider:provider_for(app)];
    NSUpdateDynamicServices();
}

bool perform_mac_service(App& app, const QString& message, const QStringList& paths)
{
    SEL selector = NSSelectorFromString([message.toNSString() stringByAppendingString:@":userData:error:"]);
    SatchelServices* provider = provider_for(app);
    if (![provider respondsToSelector:selector])
        return false;
    NSPasteboard* pboard = [NSPasteboard pasteboardWithUniqueName];
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (const auto& p : paths)
        [urls addObject:[NSURL fileURLWithPath:p.toNSString()]];
    [pboard clearContents];
    [pboard writeObjects:urls];
    NSString* error = nil;
    if (message == "compressFiles")
        [provider compressFiles:pboard userData:@"" error:&error];
    else
        [provider extractFiles:pboard userData:@"" error:&error];
    [pboard releaseGlobally];
    return error == nil;
}
