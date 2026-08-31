// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_THEME_H
#define CHIAKI_THEME_H

// Applies CloudPad's Akira-derived color palette to Borealis's light/dark
// theme tables. Must run after brls::Application::init() (Theme::
// getLightTheme()/getDarkTheme() are Application-owned) and before the
// first view is pushed. Ported from xlanor/akira's source/ui/theme.cpp
// kPlayStation ("Cobalt") palette and applyToBorealis() - the source of the
// "akira/highlight/..." glow color keys core/view.cpp's focus-ring
// rendering looks up (and fatals if missing, which is what surfaced this).
void ApplyCloudPadTheme();

#endif // CHIAKI_THEME_H
