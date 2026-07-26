// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "views/cloud_game_list.h"

#include <fmt/format.h>

#include "cloudcatalog.h"

CloudGameList::CloudGameList(Settings *settings, ChiakiLog *log, std::string platform)
{
	CloudCatalog catalog(log);
	std::vector<CloudGame> games;
	std::string error;
	bool ok;

	brls::Application::blockInputs();
	if(platform == "ps5")
	{
		ok = catalog.FetchPs5CloudCatalog("en-US", &games, &error);
	}
	else
	{
		std::string npsso = settings->GetNPSSO();
		std::string duid = settings->GetOrCreateDUID();
		std::vector<CloudGame> all_games;
		ok = catalog.FetchPsnowCatalog(npsso, duid, &all_games, &error);
		if(ok)
		{
			for(const auto &g : all_games)
				if(g.platform == platform)
					games.push_back(g);
		}
	}
	brls::Application::unblockInputs();

	brls::ListItem *status = new brls::ListItem(platform == "ps5" ? "PS5 Cloud Catalog" : "PSNOW Catalog");
	status->setValue(ok ? fmt::format("{} games", games.size()) : "Failed to load");
	this->addView(status);

	if(!ok)
	{
		CHIAKI_LOGE(log, "CloudGameList: failed to load %s catalog: %s", platform.c_str(), error.c_str());
		brls::Label *error_label = new brls::Label(brls::LabelStyle::REGULAR, error, true);
		this->addView(error_label);
		return;
	}

	for(const auto &game : games)
	{
		brls::ListItem *item = new brls::ListItem(game.name.empty() ? game.product_id : game.name);
		item->getClickEvent()->subscribe([](brls::View *view) {
			brls::Application::notify("Cloud streaming isn't wired up yet - coming in the next update");
		});
		this->addView(item);
	}
}
