// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Satchel contributors
#include "mac_drag.hpp"

#include <QImage>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <utility>

@interface SatchelPromiseDrag : NSObject <NSFilePromiseProviderDelegate, NSDraggingSource>
@end

namespace
{

NSMutableSet<SatchelPromiseDrag*>* live_drags()
{
    static NSMutableSet<SatchelPromiseDrag*>* set = [NSMutableSet set];
    return set;
}

} // namespace

@implementation SatchelPromiseDrag
{
   @public
    QList<PromisedItem> items;
    std::function<void()> ended;
    NSArray<NSFilePromiseProvider*>* providers;
    __weak NSWindow* window;
    int pending;
    bool session_over;
}

- (void)forgetIfDone
{
    if (session_over && pending == 0)
        [live_drags() removeObject:self];
}

- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider fileNameForType:(NSString*)fileType
{
    (void)fileType;
    return items.value([provider.userInfo integerValue]).name.toNSString();
}

- (NSOperationQueue*)operationQueueForFilePromiseProvider:(NSFilePromiseProvider*)provider
{
    (void)provider;
    return NSOperationQueue.mainQueue;
}

- (void)filePromiseProvider:(NSFilePromiseProvider*)provider writePromiseToURL:(NSURL*)url completionHandler:(void (^)(NSError*))completionHandler
{
    const auto index = static_cast<qsizetype>([provider.userInfo integerValue]);
    void (^handler)(NSError*) = [completionHandler copy];
    if (index < 0 || index >= items.size() || !items[index].write)
    {
        handler([NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]);
        return;
    }
    ++pending;
    SatchelPromiseDrag* keep = self;
    items[index].write(QString::fromNSString(url.path), [keep, handler](const QString& error)
                       {
        NSError* e = nil;
        if (!error.isEmpty())
            e = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:@{ NSLocalizedDescriptionKey: error.toNSString() }];
        handler(e);
        --keep->pending;
        [keep forgetIfDone]; });
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void)session;
    return context == NSDraggingContextWithinApplication ? (NSDragOperationCopy | NSDragOperationMove | NSDragOperationGeneric) : NSDragOperationCopy;
}

- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
    (void)session;
    (void)point;
    (void)operation;
    session_over = true;
    if (NSWindow* w = window)
    {
        const NSPoint at = [w convertPointFromScreen:point];
        if (NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:at modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:w.windowNumber context:nil eventNumber:0 clickCount:1 pressure:0])
            [w sendEvent:up];
    }
    if (auto end = std::exchange(ended, nullptr))
        end();
    [self forgetIfDone];
}

@end

bool start_promise_drag(QWidget* source, QList<PromisedItem> items, const QPixmap& image, std::function<void()> ended)
{
    if (!source || items.isEmpty())
        return false;
    NSEvent* event = NSApp.currentEvent;
    if (!event || (event.type != NSEventTypeLeftMouseDragged && event.type != NSEventTypeLeftMouseDown))
        return false;
    auto* view = (__bridge NSView*)reinterpret_cast<void*>(source->window()->winId());
    if (!view)
        return false;

    SatchelPromiseDrag* drag = [[SatchelPromiseDrag alloc] init];
    drag->ended = std::move(ended);
    drag->pending = 0;
    drag->session_over = false;
    drag->window = view.window;
    NSMutableSet<NSPasteboardType>* types = [NSMutableSet setWithArray:view.registeredDraggedTypes];
    [types addObjectsFromArray:NSFilePromiseReceiver.readableDraggedTypes];
    [view registerForDraggedTypes:types.allObjects];

    NSImage* picture = nil;
    NSSize size = NSMakeSize(32, 32);
    if (!image.isNull())
    {
        const auto cg = image.toImage().toCGImage();
        size = NSMakeSize(image.width() / image.devicePixelRatio(), image.height() / image.devicePixelRatio());
        picture = [[NSImage alloc] initWithCGImage:cg size:size];
        CGImageRelease(cg);
    }
    const NSPoint at = [view convertPoint:event.locationInWindow fromView:nil];
    const NSRect frame = NSMakeRect(at.x - size.width / 2, at.y - size.height / 2, size.width, size.height);

    NSMutableArray<NSDraggingItem*>* dragging = [NSMutableArray array];
    NSMutableArray<NSFilePromiseProvider*>* providers = [NSMutableArray array];
    for (qsizetype i = 0; i < items.size(); ++i)
    {
        NSString* type = items[i].folder ? UTTypeFolder.identifier : UTTypeData.identifier;
        NSFilePromiseProvider* provider = [[NSFilePromiseProvider alloc] initWithFileType:type delegate:drag];
        provider.userInfo = @(i);
        [providers addObject:provider];
        NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:provider];
        [item setDraggingFrame:frame contents:i == 0 ? picture : nil];
        [dragging addObject:item];
    }
    drag->items = std::move(items);
    drag->providers = providers;
    [live_drags() addObject:drag];
    NSDraggingSession* session = [view beginDraggingSessionWithItems:dragging event:event source:drag];
    if (!session)
    {
        [live_drags() removeObject:drag];
        return false;
    }
    session.animatesToStartingPositionsOnCancelOrFail = YES;
    return true;
}
