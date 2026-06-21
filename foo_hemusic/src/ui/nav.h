#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Page navigation stack for the HE-Music panel (PLAN.md Phase 4: ui/nav.h).
//
// Pure data + a stack discipline; no SDK, no rendering. Caller pushes a
// PageEntry when the user opens a detail page, pops on back. The trail is
// what the breadcrumb at the top of the panel renders ("歌单 > XX 的歌单").
//
// Single-threaded: only the UI thread touches Stack. Callers redraw after
// their own push/pop, so there's no listener API.

namespace hemusic::ui::nav {

// Closed enumeration of the pages the panel can navigate to (PLAN §5.1-5.8).
// New entries here mean the panel's renderer also grows a case, so the
// enum-as-type-tag is the friction we want.
enum class PageKind : std::uint8_t {
    Discover,
    Search,
    PlaylistDetail,
    AlbumDetail,
    ArtistDetail,
    Ranking,
    Radio,
    My,
};

struct PageEntry {
    PageKind kind = PageKind::Discover;
    std::string title;  // breadcrumb label
    // Opaque to nav. Renderer's contract for what keys belong here (e.g.
    // PlaylistDetail needs "id" and "platform"). Kept as a flat string map so
    // back-navigation can restore the page without nav knowing per-kind types.
    std::unordered_map<std::string, std::string> params;
};

class Stack {
   public:
    Stack() = default;

    // Discards any prior state and seats `entry` as the new root. Use on
    // tab switch / logout.
    void replaceRoot(PageEntry entry);

    // Pushes a child page above the current top. When the stack is empty()
    // the entry is seated as the new root instead (keeps the state usable
    // rather than silently dropping the call -- there is no path in the UI
    // where pushing onto nothing should fail quietly).
    void push(PageEntry entry);

    // Pops the top entry. Returns it on success; returns nullopt when the
    // stack is at the root (or empty) -- caller's "back" button should be
    // disabled in that state.
    std::optional<PageEntry> pop();

    bool canGoBack() const { return entries_.size() > 1; }
    std::size_t depth() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // Top of stack. Precondition: !empty(). Caller guards with empty().
    const PageEntry& current() const { return entries_.back(); }

    // Root-to-current breadcrumb trail. Empty when empty().
    const std::vector<PageEntry>& trail() const { return entries_; }

    void clear() { entries_.clear(); }

   private:
    std::vector<PageEntry> entries_;
};

}  // namespace hemusic::ui::nav
