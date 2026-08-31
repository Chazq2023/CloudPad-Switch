// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "theme.h"

#include <borealis.hpp>

namespace
{
	// akira's default "Cobalt" palette (source/ui/theme.cpp's kPlayStation).
	// Kept as a plain struct of NVGcolor rather than pulling in akira's full
	// Palette type/multi-theme switcher (id/name/silver/gold/bronze etc. -
	// unused here since this app doesn't have a theme-picker setting yet).
	struct Palette
	{
		NVGcolor background, backgroundDeep, gradientTop, gradientBottom;
		NVGcolor surface, surfaceElevated, surfaceLine;
		NVGcolor accent, accentStrong, focusA, focusB;
		NVGcolor text, textMuted, textDim;
		NVGcolor success, warning, danger, media;
	};

	const Palette kCobalt = {
		.background      = nvgRGB(0x0b, 0x1a, 0x3a),
		.backgroundDeep  = nvgRGB(0x08, 0x0f, 0x20),
		.gradientTop     = nvgRGB(0x18, 0x33, 0x6e),
		.gradientBottom  = nvgRGB(0x05, 0x08, 0x12),
		.surface         = nvgRGB(0x16, 0x30, 0x7a),
		.surfaceElevated = nvgRGB(0x1e, 0x45, 0xa8),
		.surfaceLine     = nvgRGBA(0xff, 0xff, 0xff, 0x17),
		.accent          = nvgRGB(0x4c, 0x9b, 0xff),
		.accentStrong    = nvgRGB(0x2e, 0x7b, 0xf6),
		.focusA          = nvgRGB(0x6f, 0xb3, 0xff),
		.focusB          = nvgRGB(0xdc, 0xeb, 0xff),
		.text            = nvgRGB(0xff, 0xff, 0xff),
		.textMuted       = nvgRGB(0xae, 0xc2, 0xe6),
		.textDim         = nvgRGB(0x6b, 0x7c, 0xa0),
		.success         = nvgRGB(0x3b, 0xc7, 0x7a),
		.warning         = nvgRGB(0xf5, 0xa6, 0x23),
		.danger          = nvgRGB(0xe5, 0x48, 0x4d),
		.media           = nvgRGB(0xb1, 0x5c, 0xe0),
	};

	NVGcolor WithAlpha(NVGcolor color, unsigned char alpha)
	{
		return nvgRGBA(
			(unsigned char)(color.r * 255.0f),
			(unsigned char)(color.g * 255.0f),
			(unsigned char)(color.b * 255.0f),
			alpha);
	}
}

void ApplyCloudPadTheme()
{
	const Palette &p = kCobalt;

	// Cobalt is a single dark palette - both Borealis theme variants get the
	// same colors, matching akira's own set() helper (which always writes
	// both getLightTheme() and getDarkTheme()) since this app has no
	// separate light/dark toggle of its own.
	auto set = [](const char *key, NVGcolor color) {
		brls::Theme::getLightTheme().addColor(key, color);
		brls::Theme::getDarkTheme().addColor(key, color);
	};

	set("brls/background", p.background);
	set("akira/gradient_top", p.gradientTop);
	set("akira/gradient_bottom", p.gradientBottom);
	set("brls/text", p.text);
	set("brls/text_disabled", p.textDim);
	set("brls/click_pulse", WithAlpha(p.accent, 0x26));

	set("brls/applet_frame/separator", p.surfaceLine);
	set("brls/header/border", p.surfaceLine);
	set("brls/header/rectangle", p.textMuted);
	set("brls/header/subtitle", p.textDim);

	set("brls/button/primary_enabled_background", p.accentStrong);
	set("brls/button/primary_disabled_background", p.surfaceLine);
	set("brls/button/primary_enabled_text", p.text);
	set("brls/button/primary_disabled_text", p.textDim);
	set("brls/button/default_enabled_background", p.surface);
	set("brls/button/default_disabled_background", p.surface);
	set("brls/button/default_enabled_text", p.text);
	set("brls/button/default_disabled_text", p.textDim);
	set("brls/button/highlight_enabled_text", p.accent);
	set("brls/button/highlight_disabled_text", p.accent);
	set("brls/button/enabled_border_color", p.surfaceLine);
	set("brls/button/disabled_border_color", p.surfaceLine);

	set("brls/slider/line_filled", p.accentStrong);
	set("brls/slider/line_empty", p.surfaceLine);
	set("brls/slider/pointer_color", p.text);
	set("brls/slider/pointer_border_color", p.accent);

	set("brls/sidebar/background", p.backgroundDeep);
	set("brls/sidebar/active_item", p.accent);
	set("brls/sidebar/separator", p.surfaceLine);

	set("brls/list/listItem_value_color", p.accent);

	set("brls/highlight/color1", p.focusA);
	set("brls/highlight/color2", p.focusB);
	set("brls/highlight/background", p.background);

	// Cobalt isn't one of akira's "logoSelector" (ps30/ps30-light) themes,
	// so the focus-ring glow stays a flat focusB tint rather than the
	// multi-color glow those themes use - matches akira's own glowOr()
	// fallback exactly.
	set("akira/highlight/multiglow", nvgRGBA(0, 0, 0, 0x00));
	set("akira/highlight/glow1", p.focusB);
	set("akira/highlight/glow2", p.focusB);
	set("akira/highlight/glow3", p.focusB);
	set("akira/highlight/glow4", p.focusB);
	set("akira/highlight/ribbon1", p.focusB);
	set("akira/highlight/ribbon2", p.focusB);
	set("akira/highlight/ribbon3", p.focusB);
	set("akira/highlight/ribbon4", p.focusB);

	set("brls/spinner/bar_color", WithAlpha(p.accent, 0x50));

	set("color/card", p.surface);
	set("color/grey_3", p.surfaceElevated);
}
