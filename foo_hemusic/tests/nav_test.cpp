#include "ui/nav.h"

#include <catch2/catch_test_macros.hpp>

using hemusic::ui::nav::PageEntry;
using hemusic::ui::nav::PageKind;
using hemusic::ui::nav::Stack;

namespace {

PageEntry mk(PageKind k, std::string title) {
    PageEntry e;
    e.kind = k;
    e.title = std::move(title);
    return e;
}

}  // namespace

TEST_CASE("empty stack has no current and forbids back") {
    Stack s;
    CHECK(s.empty());
    CHECK(s.depth() == 0);
    CHECK_FALSE(s.canGoBack());
    CHECK(s.trail().empty());
    CHECK_FALSE(s.pop().has_value());
}

TEST_CASE("replaceRoot seats a single page; back stays disabled") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    REQUIRE(s.depth() == 1);
    CHECK_FALSE(s.canGoBack());
    CHECK(s.current().kind == PageKind::Discover);
    CHECK(s.current().title == "发现");
}

TEST_CASE("push grows the stack and enables back") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    s.push(mk(PageKind::PlaylistDetail, "歌单"));

    REQUIRE(s.depth() == 2);
    CHECK(s.canGoBack());
    CHECK(s.current().kind == PageKind::PlaylistDetail);
    CHECK(s.trail().front().kind == PageKind::Discover);
    CHECK(s.trail().back().kind == PageKind::PlaylistDetail);
}

TEST_CASE("pop returns top and falls back to root") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    s.push(mk(PageKind::Search, "搜索"));
    s.push(mk(PageKind::PlaylistDetail, "歌单详情"));

    auto popped = s.pop();
    REQUIRE(popped.has_value());
    CHECK(popped->kind == PageKind::PlaylistDetail);
    CHECK(s.current().kind == PageKind::Search);

    popped = s.pop();
    REQUIRE(popped.has_value());
    CHECK(popped->kind == PageKind::Search);
    CHECK(s.current().kind == PageKind::Discover);

    // At root: pop is a no-op, back disabled.
    CHECK_FALSE(s.canGoBack());
    CHECK_FALSE(s.pop().has_value());
    CHECK(s.current().kind == PageKind::Discover);
}

TEST_CASE("replaceRoot discards prior trail") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    s.push(mk(PageKind::PlaylistDetail, "歌单"));
    s.push(mk(PageKind::Search, "搜索"));

    s.replaceRoot(mk(PageKind::My, "我的"));
    REQUIRE(s.depth() == 1);
    CHECK(s.current().kind == PageKind::My);
    CHECK_FALSE(s.canGoBack());
}

TEST_CASE("push on empty stack acts as replaceRoot to stay usable") {
    Stack s;
    s.push(mk(PageKind::Discover, "发现"));
    REQUIRE(s.depth() == 1);
    CHECK_FALSE(s.canGoBack());
    CHECK(s.current().kind == PageKind::Discover);
}

TEST_CASE("clear resets to empty state") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    s.push(mk(PageKind::Search, "搜索"));
    s.clear();
    CHECK(s.empty());
    CHECK_FALSE(s.canGoBack());
    CHECK_FALSE(s.pop().has_value());
}

TEST_CASE("params travel with the entry through push and pop") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    PageEntry detail = mk(PageKind::PlaylistDetail, "歌单详情");
    detail.params["id"] = "playlist-42";
    detail.params["platform"] = "LinuxDo";
    s.push(detail);

    CHECK(s.current().params.at("id") == "playlist-42");
    auto popped = s.pop();
    REQUIRE(popped.has_value());
    CHECK(popped->params.at("platform") == "LinuxDo");
    CHECK(s.current().kind == PageKind::Discover);
}

TEST_CASE("trail order matches push order") {
    Stack s;
    s.replaceRoot(mk(PageKind::Discover, "发现"));
    s.push(mk(PageKind::Search, "搜索"));
    s.push(mk(PageKind::PlaylistDetail, "歌单详情"));

    const auto& t = s.trail();
    REQUIRE(t.size() == 3);
    CHECK(t.at(0).kind == PageKind::Discover);
    CHECK(t.at(1).kind == PageKind::Search);
    CHECK(t.at(2).kind == PageKind::PlaylistDetail);
}
