// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "mac_services.hpp"

void install_mac_services(App&) {}

bool perform_mac_service(App&, const QString&, const QStringList&)
{
    return false;
}

#include "mac_uninstall.hpp"

bool uninstall_available()
{
    return false;
}

void run_uninstall_dialog(QWidget*) {}

void add_uninstall_action(QMenu*, QWidget*) {}
