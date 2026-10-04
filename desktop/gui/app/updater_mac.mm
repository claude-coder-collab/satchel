// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "updater.hpp"

#import <Foundation/Foundation.h>

// The parts of Sparkle 2's API that are used; the framework is loaded at run time.
@protocol ZPSparkleUpdater <NSObject>
@property BOOL automaticallyChecksForUpdates;
- (BOOL)startUpdater:(NSError**)error;
@end

@protocol ZPSparkleController <NSObject>
- (instancetype)initWithStartingUpdater:(BOOL)start updaterDelegate:(id)updaterDelegate userDriverDelegate:(id)userDriverDelegate;
- (id<ZPSparkleUpdater>)updater;
- (void)checkForUpdates:(id)sender;
@end

namespace
{

class SparkleUpdater final : public Updater
{
   public:
    explicit SparkleUpdater(id<ZPSparkleController> controller) : controller_(controller)
    {
    }

    [[nodiscard]] bool available() const override { return true; }
    void check_now() override { [controller_ checkForUpdates:nil]; }
    void set_automatic(bool enabled) override { controller_.updater.automaticallyChecksForUpdates = enabled; }

   private:
    id<ZPSparkleController> controller_;
};

} // namespace

std::unique_ptr<Updater> Updater::create(bool automatic)
{
    NSString* path = [NSBundle.mainBundle.privateFrameworksPath stringByAppendingPathComponent:@"Sparkle.framework"];
    NSBundle* framework = [NSBundle bundleWithPath:path];
    Class cls = (framework && [framework load]) ? NSClassFromString(@"SPUStandardUpdaterController") : nil;
    if (!cls)
        return std::make_unique<Updater>();
    id<ZPSparkleController> controller = [(id<ZPSparkleController>)[cls alloc] initWithStartingUpdater:NO updaterDelegate:nil userDriverDelegate:nil];
    controller.updater.automaticallyChecksForUpdates = automatic;
    NSError* error = nil;
    if (![controller.updater startUpdater:&error])
    {
        NSLog(@"Sparkle could not start: %@", error);
        return std::make_unique<Updater>();
    }
    return std::make_unique<SparkleUpdater>(controller);
}
