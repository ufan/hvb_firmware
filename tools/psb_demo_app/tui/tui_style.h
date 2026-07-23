#pragma once

#include <ftxui/dom/elements.hpp>

namespace psb::tui {

inline ftxui::Element appMenuChrome(ftxui::Element element) {
    return element | ftxui::color(ftxui::Color::White)
                   | ftxui::bgcolor(ftxui::Color::RGB(18, 58, 90));
}

inline ftxui::Element appTitleChrome(ftxui::Element element) {
    return element | ftxui::bold
                   | ftxui::color(ftxui::Color::Black)
                   | ftxui::bgcolor(ftxui::Color::CyanLight);
}

inline ftxui::Element boardMenuChrome(ftxui::Element element) {
    return element | ftxui::color(ftxui::Color::YellowLight)
                   | ftxui::bgcolor(ftxui::Color::RGB(46, 42, 22));
}

inline ftxui::Element sidebarChrome(ftxui::Element element) {
    return element | ftxui::color(ftxui::Color::GrayLight)
                   | ftxui::bgcolor(ftxui::Color::RGB(22, 26, 32));
}

inline ftxui::Element sidebarTitleChrome(ftxui::Element element) {
    return element | ftxui::bold | ftxui::color(ftxui::Color::White);
}

} // namespace psb::tui
