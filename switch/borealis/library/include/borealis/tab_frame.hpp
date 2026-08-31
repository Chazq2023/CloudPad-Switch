/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019-2020  natinusala
    Copyright (C) 2019  p-sam

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <borealis/applet_frame.hpp>
#include <borealis/sidebar.hpp>
#include <string>
#include <vector>

namespace brls
{

// An applet frame containing a sidebar on the left with multiple tabs
class TabFrame : public AppletFrame
{
  public:
    TabFrame();

    /**
     * Adds a tab with given label and view
     * All tabs and separators must be added
     * before the TabFrame is itself added to
     * the view hierarchy
     *
     * Returns the SidebarItem backing this tab, so callers can later swap
     * in a freshly-built replacement view for it (via
     * SidebarItem::setAssociatedView + TabFrame::switchToView) without
     * touching BoxLayout::removeView, which isn't safe to use at runtime.
     */
    SidebarItem* addTab(std::string label, View* view);
    void addSeparator();

    View* getDefaultFocus() override;

    virtual bool onCancel() override;

    // Public so a tab's own content can request switching to a replacement
    // view for itself (see addTab's doc comment above).
    void switchToView(View* view);

    ~TabFrame();

  private:
    Sidebar* sidebar;
    BoxLayout* layout;
    View* rightPane = nullptr;
};

} // namespace brls
